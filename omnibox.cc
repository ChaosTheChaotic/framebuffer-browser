#include "omnibox.hh"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <initializer_list>
#include <utility>

namespace {

constexpr const char *kTldListUrl =
    "https://data.iana.org/TLD/tlds-alpha-by-domain.txt";
constexpr qint64 kTldMaxAgeSecs = 7 * 24 * 60 * 60;
constexpr int kTldCheckIntervalMs = 6 * 60 * 60 * 1000;
constexpr int kMinTldCount = 500; // sanity check for a downloaded list

QSet<QString> makeSet(std::initializer_list<const char *> items) {
  QSet<QString> s;
  for (const char *i : items)
    s.insert(QLatin1String(i));
  return s;
}

// Only used until the real list has been loaded
const QSet<QString> &fallbackTlds() {
  static const QSet<QString> s = makeSet(
      {"com", "org", "net", "edu", "gov", "mil", "int", "info", "biz", "name",
       "pro", "io", "dev", "app", "ai", "co", "me", "tv", "xyz", "online",
       "site", "tech", "store", "blog", "cloud", "page", "news", "shop",
       "wiki", "arpa"});
  return s;
}

// Not in the IANA root zone, but real in private networks
bool isLocalSuffix(const QString &tld) {
  static const QSet<QString> s = makeSet(
      {"localhost", "local", "internal", "lan", "test", "invalid", "example"});
  return s.contains(tld);
}

// Schemes that are URLs without "//" after the colon
bool isBareScheme(const QString &scheme) {
  static const QSet<QString> s = makeSet({"about", "blob", "data", "view-source"});
  return s.contains(scheme);
}

bool isAsciiDigit(QChar c) { return c.unicode() >= '0' && c.unicode() <= '9'; }

bool isAsciiLetter(QChar c) {
  const ushort u = c.unicode();
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
}

bool allDigits(const QString &s) {
  if (s.isEmpty())
    return false;
  for (QChar c : s)
    if (!isAsciiDigit(c))
      return false;
  return true;
}

bool isIPv4(const QString &s) {
  const QStringList parts = s.split(QLatin1Char('.'));
  if (parts.size() != 4)
    return false;
  for (const QString &p : parts) {
    if (!allDigits(p) || p.size() > 3)
      return false;
    if (p.size() > 1 && p[0] == QLatin1Char('0'))
      return false;
    if (p.toInt() > 255)
      return false;
  }
  return true;
}

bool isIPv6(const QString &s) {
  if (!s.contains(QLatin1Char(':')) || s.contains(QLatin1Char('%')))
    return false;
  QHostAddress a;
  return a.setAddress(s) && a.protocol() == QAbstractSocket::IPv6Protocol;
}

bool isValidPort(const QString &s) {
  if (!allDigits(s) || s.size() > 5)
    return false;
  const int p = s.toInt();
  return p >= 1 && p <= 65535;
}

// ascii must already be lowercase punycode
bool isValidHostname(const QString &ascii, bool allowUnderscore) {
  if (ascii.isEmpty() || ascii.size() > 253)
    return false;
  for (const QString &label : ascii.split(QLatin1Char('.'))) {
    if (label.isEmpty() || label.size() > 63)
      return false;
    if (label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-')))
      return false;
    for (QChar c : label) {
      const bool ok = isAsciiDigit(c) || (c.unicode() >= 'a' && c.unicode() <= 'z') ||
                      c == QLatin1Char('-') ||
                      (allowUnderscore && c == QLatin1Char('_'));
      if (!ok)
        return false;
    }
  }
  return true;
}

struct Authority {
  QString host; // lowercase ASCII; IPv6 without brackets
  QString port; // empty if none
  bool ipv4 = false;
  bool ipv6 = false;
};

// Parses "host", "host:port", "[v6]", "[v6]:port" or a bare IPv6 address
// Anything else (userinfo, spaces, bad ports, bad characters) is rejected
std::optional<Authority> parseAuthority(const QString &s) {
  if (s.isEmpty() || s.contains(QLatin1Char('@')))
    return std::nullopt;

  Authority a;
  QString host;
  bool hasPortSep = false;

  if (s.startsWith(QLatin1Char('['))) {
    const int end = s.indexOf(QLatin1Char(']'));
    if (end < 0)
      return std::nullopt;
    host = s.mid(1, end - 1);
    const QString rest = s.mid(end + 1);
    if (!rest.isEmpty()) {
      if (!rest.startsWith(QLatin1Char(':')))
        return std::nullopt;
      hasPortSep = true;
      a.port = rest.mid(1);
    }
    if (!isIPv6(host))
      return std::nullopt;
    a.ipv6 = true;
  } else {
    const int colons = s.count(QLatin1Char(':'));
    if (colons == 1) {
      const int i = s.indexOf(QLatin1Char(':'));
      host = s.left(i);
      a.port = s.mid(i + 1);
      hasPortSep = true;
    } else if (colons > 1) {
      if (!isIPv6(s))
        return std::nullopt;
      host = s;
      a.ipv6 = true;
    } else {
      host = s;
    }
  }

  if (hasPortSep && !isValidPort(a.port))
    return std::nullopt;
  if (a.ipv6) {
    a.host = host.toLower();
    return a;
  }

  host = host.toLower();
  if (host.endsWith(QLatin1Char('.')))
    host.chop(1);
  if (isIPv4(host)) {
    a.ipv4 = true;
    a.host = host;
    return a;
  }

  const QString ascii = QString::fromLatin1(QUrl::toAce(host)).toLower();
  // Underscores only make sense for private hosts, which need a port here;
  // this keeps file names like my_script.py out of the URL path
  if (!isValidHostname(ascii, !a.port.isEmpty()))
    return std::nullopt;
  a.host = ascii;
  return a;
}

// Local paths: ./x ../x ~/x always; /x only if it exists
std::optional<QUrl> fileUrl(const QString &input) {
  QString path = input;
  bool forced = true;
  if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")))
    path = QDir::homePath() + path.mid(1);
  else if (!path.startsWith(QLatin1String("./")) &&
           !path.startsWith(QLatin1String("../")))
    forced = false;

  if (!forced && !(path.startsWith(QLatin1Char('/')) && QFileInfo::exists(path)))
    return std::nullopt;
  return QUrl::fromLocalFile(
      QDir::cleanPath(QDir::current().absoluteFilePath(path)));
}

} // namespace

bool TldSet::load(const QByteArray &text) {
  QSet<QString> set;
  for (const QByteArray &line : text.split('\n')) {
    const QString s = QString::fromLatin1(line).trimmed().toLower();
    if (!s.isEmpty() && !s.startsWith(QLatin1Char('#')))
      set.insert(s);
  }
  if (set.size() < kMinTldCount || !set.contains(QStringLiteral("com")))
    return false;
  tlds_ = std::move(set);
  loaded_ = true;
  return true;
}

bool TldSet::contains(const QString &tld) const {
  if (loaded_)
    return tlds_.contains(tld);
	// Just accept any 2 letter label as country code
  return fallbackTlds().contains(tld) ||
         (tld.size() == 2 && isAsciiLetter(tld[0]) && isAsciiLetter(tld[1]));
}

Omnibox::Omnibox(QString searchTemplate, bool autoUpdate, QObject *parent)
    : QObject(parent), searchTemplate_(std::move(searchTemplate)),
      autoUpdate_(autoUpdate) {
  const bool schemeOk = searchTemplate_.startsWith(QLatin1String("https://")) ||
                        searchTemplate_.startsWith(QLatin1String("http://"));
  if (searchTemplate_.isEmpty() ||
      !(schemeOk && searchTemplate_.contains(QLatin1String("%s")))) {
    if (!searchTemplate_.isEmpty())
      qWarning() << "searchUrl must be an http(s) URL containing %s. Using default";
    searchTemplate_ = defaultSearchUrl();
  }
}

QString Omnibox::defaultSearchUrl() {
  return QStringLiteral("https://duckduckgo.com/?q=%s");
}

QUrl Omnibox::searchUrl(const QString &query) const {
  QString u = searchTemplate_;
  u.replace(QLatin1String("%s"),
            QString::fromLatin1(QUrl::toPercentEncoding(query)));
  return QUrl(u);
}

QUrl Omnibox::resolve(const QString &raw) const {
  const QString input = raw.trimmed();
  if (input.isEmpty())
    return QUrl();

	// "?" at start forces search
  if (input.startsWith(QLatin1Char('?'))) {
    const QString q = input.mid(1).trimmed();
    return q.isEmpty() ? QUrl() : searchUrl(q);
  }

  if (auto f = fileUrl(input))
    return *f;

  static const QRegularExpression schemeRe(
      QStringLiteral("^([A-Za-z][A-Za-z0-9+.-]*):"));
  const QRegularExpressionMatch m = schemeRe.match(input);
  if (m.hasMatch()) {
    const QString scheme = m.captured(1).toLower();
    const bool bare = isBareScheme(scheme);
    // "localhost:3000" also looks like a scheme, so unknown schemes need "//"
    if (bare || input.mid(m.capturedLength()).startsWith(QLatin1String("//"))) {
      const QUrl url(input, QUrl::TolerantMode);
      const bool usable =
          url.isValid() &&
          (bare || scheme == QLatin1String("file") || !url.host().isEmpty());
      return usable ? url : searchUrl(input);
    }
  }

  if (auto url = hostUrl(input))
    return *url;
  return searchUrl(input);
}

// Handles input without a scheme: host[:port][/path?query#fragment]
std::optional<QUrl> Omnibox::hostUrl(const QString &input) const {
  int split = -1;
  for (int i = 0; i < input.size(); ++i) {
    const QChar c = input[i];
    if (c == QLatin1Char('/') || c == QLatin1Char('?') || c == QLatin1Char('#')) {
      split = i;
      break;
    }
  }
  const QString authority = split < 0 ? input : input.left(split);
  const QString tail = split < 0 ? QString() : input.mid(split);

  const auto a = parseAuthority(authority);
  if (!a)
    return std::nullopt;

  const bool hasPort = !a->port.isEmpty();
  const QStringList labels = a->host.split(QLatin1Char('.'));
  bool isUrl = false;
  bool secure = false;

  if (a->ipv4 || a->ipv6) {
    isUrl = true;
  } else if (labels.size() == 1) {
    // Bare words are searches unless they are clearly a host
    isUrl = labels[0] == QLatin1String("localhost") || hasPort ||
            tail == QLatin1String("/");
  } else {
    const QString &tld = labels.last();
    const bool local = isLocalSuffix(tld);
    isUrl = !allDigits(tld) && (hasPort || local || tlds_.contains(tld));
    secure = !hasPort && !local;
  }
  if (!isUrl)
    return std::nullopt;

  QString auth = a->ipv6 ? QStringLiteral("[") + a->host + QStringLiteral("]")
                         : a->host;
  if (hasPort)
    auth += QStringLiteral(":") + a->port;

  const QUrl url(QString::fromLatin1(secure ? "https" : "http") +
                     QStringLiteral("://") + auth + tail,
                 QUrl::TolerantMode);
  if (!url.isValid())
    return std::nullopt;
  return url;
}

QString Omnibox::cachePath() {
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
         QStringLiteral("/tlds.txt");
}

void Omnibox::start() {
  QFile f(cachePath());
  if (f.open(QIODevice::ReadOnly) && !tlds_.load(f.readAll()))
    qWarning() << "Ignoring invalid TLD cache" << f.fileName();

  if (!autoUpdate_)
    return;
  connect(&refreshTimer_, &QTimer::timeout, this, &Omnibox::refreshIfStale);
  refreshTimer_.start(kTldCheckIntervalMs);
  refreshIfStale();
}

void Omnibox::refreshIfStale() {
  if (downloading_)
    return;
  const QFileInfo fi(cachePath());
  const bool fresh =
      fi.exists() &&
      fi.lastModified().secsTo(QDateTime::currentDateTime()) < kTldMaxAgeSecs;
  if (tlds_.loaded() && fresh)
    return;
  download();
}

void Omnibox::download() {
  if (!nam_)
    nam_ = new QNetworkAccessManager(this);

  QNetworkRequest req{QUrl(QLatin1String(kTldListUrl))};
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setTransferTimeout(30000);

  downloading_ = true;
  QNetworkReply *reply = nam_->get(req);
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    downloading_ = false;
    if (reply->error() != QNetworkReply::NoError) {
      qWarning() << "TLD list download failed:" << reply->errorString();
      return;
    }
    const QByteArray body = reply->readAll();
    if (!tlds_.load(body)) {
      qWarning() << "Downloaded TLD list looks invalid. Keeping the old one";
      return;
    }
    QDir().mkpath(QFileInfo(cachePath()).absolutePath());
    QSaveFile out(cachePath());
    if (out.open(QIODevice::WriteOnly)) {
      out.write(body);
      out.commit();
    }
  });
}
