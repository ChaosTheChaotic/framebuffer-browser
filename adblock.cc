#include "adblock.hh"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace {

constexpr qint64 kMinTtlSecs = 6 * 3600;       // never poll a list faster
constexpr qint64 kMaxTtlSecs = 30 * 24 * 3600; // or let it rot longer
constexpr qint64 kMinListBytes = 64;           // below this it is not a list
constexpr qint64 kMaxListBytes = 64 * 1024 * 1024;
constexpr int kCheckIntervalMs = 60 * 60 * 1000; // poll for expiry hourly
constexpr int kFirstRetryMs = 15 * 1000;         // offline at boot? retry soon,
constexpr int kMaxRetryMs = 60 * 60 * 1000;      // then back off up to 1 h
constexpr int kTransferTimeoutMs = 30 * 1000;

const char *typeName(QWebEngineUrlRequestInfo::ResourceType t) {
  using R = QWebEngineUrlRequestInfo;
  switch (t) {
  case R::ResourceTypeSubFrame:
    return "subdocument";
  case R::ResourceTypeStylesheet:
    return "stylesheet";
  case R::ResourceTypeScript:
    return "script";
  case R::ResourceTypeImage:
    return "image";
  case R::ResourceTypeFontResource:
    return "font";
  case R::ResourceTypeMedia:
    return "media";
  case R::ResourceTypeObject:
  case R::ResourceTypePluginResource:
    return "object";
  case R::ResourceTypeXhr:
    return "xmlhttprequest";
  case R::ResourceTypePing:
  case R::ResourceTypeCspReport:
    return "ping";
  default:
    return "other";
  }
}

QString cachePathFor(const QString &listDir, const QString &url) {
  QString base = QFileInfo(QUrl(url).path()).fileName();
  base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
               QStringLiteral("_"));
  if (base.isEmpty())
    base = QStringLiteral("list.txt");
  const QByteArray h =
      QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1)
          .toHex()
          .left(10);
  return listDir + QLatin1Char('/') + QString::fromLatin1(h) +
         QLatin1Char('-') + base;
}

qint64 ttlFromHeader(const QByteArray &body, qint64 fallbackSecs) {
  static const QRegularExpression re(
      QStringLiteral(R"(^!\s*Expires:\s*(\d+)\s*(day|hour))"),
      QRegularExpression::MultilineOption |
          QRegularExpression::CaseInsensitiveOption);
  const QString head = QString::fromLatin1(
      body.constData(), int(std::min<qsizetype>(body.size(), 4096)));
  qint64 secs = fallbackSecs;
  const QRegularExpressionMatch m = re.match(head);
  if (m.hasMatch()) {
    const bool days =
        m.captured(2).startsWith(QLatin1String("day"), Qt::CaseInsensitive);
    secs = m.captured(1).toLongLong() * (days ? 86400 : 3600);
  }
  return std::clamp(secs, kMinTtlSecs, kMaxTtlSecs);
}

MetaMap loadMeta(const QString &path) {
  MetaMap out;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return out;
  const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
  for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
    const QJsonObject o = it.value().toObject();
    ListMeta m;
    m.etag = o.value(QStringLiteral("etag")).toString();
    m.lastModified = o.value(QStringLiteral("lastModified")).toString();
    m.fetched = qint64(o.value(QStringLiteral("fetched")).toDouble());
    m.ttl = qint64(o.value(QStringLiteral("ttl")).toDouble());
    out.insert(it.key(), m);
  }
  return out;
}

bool writeAtomic(const QString &path, const QByteArray &data) {
  QSaveFile f(path); // temp file + rename so a crash never leaves a torn list
  if (!f.open(QIODevice::WriteOnly))
    return false;
  if (f.write(data) != data.size()) {
    f.cancelWriting();
    return false;
  }
  return f.commit();
}

void saveMeta(const QString &path, const MetaMap &meta) {
  QJsonObject root;
  for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) {
    QJsonObject o;
    o.insert(QStringLiteral("etag"), it->etag);
    o.insert(QStringLiteral("lastModified"), it->lastModified);
    o.insert(QStringLiteral("fetched"), double(it->fetched));
    o.insert(QStringLiteral("ttl"), double(it->ttl));
    root.insert(it.key(), o);
  }
  writeAtomic(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// Guards against caching a captive portal page or an error document.
bool looksLikeFilterList(const QByteArray &body) {
  if (body.size() < kMinListBytes)
    return false;
  const QByteArray head = body.left(1024).toLower();
  return !head.contains("<html") && !head.contains("<!doctype");
}

QByteArray localSignature(const QStringList &dirs) {
  QCryptographicHash h(QCryptographicHash::Sha1);
  for (const QString &path : dirs) {
    const QDir dir(path);
    if (!dir.exists())
      continue;
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files) {
      QByteArray line = fi.absoluteFilePath().toUtf8();
      line += '|';
      line += QByteArray::number(fi.size());
      line += '|';
      line += QByteArray::number(fi.lastModified().toMSecsSinceEpoch());
      line += '\n';
      h.addData(line);
    }
  }
  return h.result();
}

// Persist fresh downloads and if anything the engine is built from changed,
// read every list, build the engine and swap it in. The initial load (empty
// fresh) always builds. A refresh cycle in which every list answered 304 Not
// Modified, and no local file changed, leaves the running engine alone as
// rebuilding would burn a second of CPU and a transient second copy of the
// engine's memory to produce an identical result.
JobResult runJob(const AdBlockConfig &cfg, const QList<Fetched> &fresh,
                 const QByteArray &prevLocalSig, Fbbadblock *engine) {
  JobResult out;
  const bool haveCache = !cfg.cacheDir.isEmpty();
  const QString listDir = cfg.cacheDir + QStringLiteral("/lists");
  const QString metaPath = cfg.cacheDir + QStringLiteral("/meta.json");

  MetaMap meta;
  bool metaDirty = false;
  bool listsChanged = fresh.isEmpty();
  if (haveCache) {
    meta = loadMeta(metaPath);
    if (!fresh.isEmpty())
      QDir().mkpath(listDir);
    for (const Fetched &f : fresh) {
      ListMeta m = meta.value(f.url);
      if (f.notModified) {
        m.fetched = f.fetchedAt; // etag / ttl stay as they were
      } else {
        if (!writeAtomic(cachePathFor(listDir, f.url), f.body)) {
          qWarning() << "adblock: cannot write cache file for" << f.url;
          continue;
        }
        m.etag = f.etag;
        m.lastModified = f.lastModified;
        m.fetched = f.fetchedAt;
        m.ttl = ttlFromHeader(f.body, qint64(cfg.ttlHours) * 3600);
        listsChanged = true;
      }
      meta.insert(f.url, m);
      metaDirty = true;
    }
  }

  out.localSig = localSignature(cfg.localDirs);
  if (!listsChanged && out.localSig == prevLocalSig &&
      fbb_adblock_ready(engine)) {
    if (metaDirty)
      saveMeta(metaPath, meta); // renewed timestamps still have to persist
    out.meta = std::move(meta);
    out.unchanged = true;
    return out;
  }

  QByteArray all;
  auto append = [&](const QByteArray &data) {
    all += data;
    if (!data.endsWith('\n'))
      all += '\n';
    ++out.lists;
  };

  QByteArray resources; // scriptlet bodies (not part of the rule text)
  if (haveCache) {
    for (const QString &url : cfg.lists) {
      QFile file(cachePathFor(listDir, url));
      if (!file.open(QIODevice::ReadOnly)) {
        // Metadata without a file would make us skip the download forever.
        metaDirty |= meta.remove(url) > 0;
        continue;
      }
      append(file.readAll());
    }
    if (!cfg.resourcesUrl.isEmpty()) {
      QFile file(cachePathFor(listDir, cfg.resourcesUrl));
      if (file.open(QIODevice::ReadOnly))
        resources = file.readAll();
      else
        metaDirty |= meta.remove(cfg.resourcesUrl) > 0;
    }
    const QStringList wanted = cfg.sources();
    for (auto it = meta.begin(); it != meta.end();) { // lists no longer wanted
      if (wanted.contains(it.key())) {
        ++it;
      } else {
        QFile::remove(cachePathFor(listDir, it.key()));
        it = meta.erase(it);
        metaDirty = true;
      }
    }
    if (metaDirty)
      saveMeta(metaPath, meta);
  }

  for (const QString &path : cfg.localDirs) {
    const QDir dir(path);
    if (!dir.exists())
      continue;
    for (const QString &name :
         dir.entryList({QStringLiteral("*.txt")}, QDir::Files, QDir::Name)) {
      QFile file(dir.filePath(name));
      if (file.open(QIODevice::ReadOnly))
        append(file.readAll());
    }
  }

  if (!all.isEmpty()) {
    out.built =
        fbb_adblock_load(engine, all.constData(), size_t(all.size()),
                         resources.constData(), size_t(resources.size()));
    if (out.built) {
      // Same worker thread, same engine, ask what each scriptlet host needs.
      for (const QString &host : cfg.scriptletHosts) {
        const QByteArray url = "https://" + host.toUtf8() + "/";
        char *js = fbb_adblock_page_script(engine, url.constData());
        if (!js)
          continue;
        out.scripts.append(HostScript{host, QString::fromUtf8(js)});
        fbb_adblock_string_free(js);
      }
    }
  }
  out.meta = std::move(meta);
  return out;
}

} // namespace

QStringList AdBlockConfig::defaultLists() {
  const QString cdn =
      QStringLiteral("https://ublockorigin.github.io/uAssetsCDN/");
  return {
      cdn + QStringLiteral("filters/filters.min.txt"), // uBlock filters - Ads
      cdn +
          QStringLiteral("filters/privacy.min.txt"), // uBlock filters - Privacy
      cdn + QStringLiteral("filters/badware.txt"),   // Badware risks
      cdn + QStringLiteral("filters/quick-fixes.txt"), // Quick fixes
      cdn + QStringLiteral("filters/unbreak.min.txt"), // Unbreak
      cdn + QStringLiteral("thirdparties/easylist.txt"),
      cdn + QStringLiteral("thirdparties/easyprivacy.txt"),
  };
}

QString AdBlockConfig::defaultResourcesUrl() {
  // Not full scriptlet set
  // Only holds Braves resources and redirects, other rules are just dropped
  // Youtube does NOT depend on this
  return QStringLiteral("https://raw.githubusercontent.com/brave/"
                        "adblock-resources/master/dist/resources.json");
}

QStringList AdBlockConfig::defaultScriptletHosts() {
  return {
      QStringLiteral("www.youtube.com"),
      QStringLiteral("m.youtube.com"),
      QStringLiteral("music.youtube.com"),
      QStringLiteral("www.youtube-nocookie.com"),
  };
}

QString AdBlockConfig::defaultCacheDir() {
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  return base.isEmpty() ? QString() : base + QStringLiteral("/adblock");
}

AdBlockInterceptor::AdBlockInterceptor() : engine_(fbb_adblock_new()) {}

AdBlockInterceptor::~AdBlockInterceptor() {
  fbb_adblock_free(engine_);
  engine_ = nullptr;
}

void AdBlockInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
  // No engine yet (still loading)
  // Let it through instead of stalling the UI.
  if (!ready())
    return;
  if (info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
    return;
  const QUrl url = info.requestUrl();
  if (url.scheme() != QLatin1String("http") &&
      url.scheme() != QLatin1String("https"))
    return;
  if (fbb_adblock_should_block(engine_, url.toEncoded().constData(),
                               info.firstPartyUrl().toEncoded().constData(),
                               typeName(info.resourceType())))
    info.block(true);
}

AdBlockManager::AdBlockManager(AdBlockConfig cfg, QObject *parent)
    : QObject(parent), cfg_(std::move(cfg)),
      interceptor_(new AdBlockInterceptor), retryMs_(kFirstRetryMs) {
  refreshTimer_.setSingleShot(true);
  connect(&refreshTimer_, &QTimer::timeout, this,
          &AdBlockManager::checkForUpdates);
  connect(&watcher_, &QFutureWatcher<JobResult>::finished, this,
          &AdBlockManager::onJobFinished);
}

AdBlockManager::~AdBlockManager() {
  watcher_.waitForFinished();
  // MainWindow detaches the interceptor from the profile before this runs.
  delete interceptor_;
}

void AdBlockManager::start() {
  installYoutubeScript(); // before the first navigation, needs no lists
  startJob({});
}

void AdBlockManager::startJob(QList<Fetched> fresh) {
  queued_ += fresh;
  // Our own flag rather than watcher_.isRunning()
  // The future can be finished while its finished() signal is still queued, and
  // calling setFuture() then
  // would drop that signal.
  if (jobRunning_)
    return; // onJobFinished() picks up queued_
  jobRunning_ = true;
  QList<Fetched> batch = std::exchange(queued_, {});
  watcher_.setFuture(QtConcurrent::run(
      [cfg = cfg_, batch = std::move(batch), sig = std::move(localSig_),
       h = interceptor_->handle()] { return runJob(cfg, batch, sig, h); }));
}

void AdBlockManager::onJobFinished() {
  jobRunning_ = false;
  const JobResult r = watcher_.result();
  meta_ = r.meta;
  localSig_ = r.localSig;

  if (r.built) {
    qInfo() << "adblock: engine ready -" << r.lists << "lists";
    // Before settled(), the first page must already find them installed.
    installScripts(r.scripts);
  } else if (!r.unchanged) {
    if (r.lists == 0)
      qInfo() << "adblock: no filter lists available (yet)";
    else
      qWarning() << "adblock: building the engine failed";
  }

  if (!firstJobDone_) {
    firstJobDone_ = true;
    emit settled();
  }

  if (!queued_.isEmpty()) { // downloads arrived while we were building
    startJob({});
    return;
  }
  if (cfg_.download && !updatesStarted_) {
    updatesStarted_ = true;
    checkForUpdates(); // now that we know what the cache contains
  }
}

void AdBlockManager::installScripts(const QList<HostScript> &scripts) {
  static const QString prefix = QStringLiteral("fbb-adblock:");
  QWebEngineScriptCollection *coll =
      QWebEngineProfile::defaultProfile()->scripts();
  // Replace what an earlier build installed
  // Pages that are already loaded keep running the old script, the next
  // navigation picks up the new one.
  const QList<QWebEngineScript> old = coll->toList();
  for (const QWebEngineScript &s : old)
    if (s.name().startsWith(prefix))
      coll->remove(s);

  static const QRegularExpression safeHost(QStringLiteral("^[A-Za-z0-9.-]+$"));
  for (const HostScript &hs : scripts) {
    if (hs.js.trimmed().isEmpty() || !safeHost.match(hs.host).hasMatch())
      continue;
    QWebEngineScript s;
    s.setName(prefix + hs.host);
    s.setInjectionPoint(QWebEngineScript::DocumentCreation); // before page JS
    s.setWorldId(QWebEngineScript::MainWorld); // scriptlets patch page globals
    s.setRunsOnSubFrames(true);                // embeds live in iframes
    s.setSourceCode(QStringLiteral("(function(){if(location.hostname!==\"") +
                    hs.host + QStringLiteral("\")return;\n") + hs.js +
                    QStringLiteral("\n})();"));
    coll->insert(s);
  }
}

void AdBlockManager::installYoutubeScript() {
  // "adblockScriptletHosts": [] (no YouTube host) is the off switch.
  const bool wanted = std::any_of(
      cfg_.scriptletHosts.cbegin(), cfg_.scriptletHosts.cend(),
      [](const QString &h) { return h.contains(QLatin1String("youtube")); });
  if (!wanted)
    return;
  QFile file(QStringLiteral(":/adblock/youtube.js"));
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "adblock: the YouTube script is missing from the resources";
    return;
  }
  // installScripts() only ever removes "fbb-adblock:" scripts,
  // so this one survives every engine rebuild. It checks the hostname itself.
  QWebEngineScript s;
  s.setName(QStringLiteral("fbb-youtube"));
  s.setInjectionPoint(QWebEngineScript::DocumentCreation); // before page JS
  s.setWorldId(QWebEngineScript::MainWorld);
  s.setRunsOnSubFrames(true); // embedded players are iframes
  s.setSourceCode(QString::fromUtf8(file.readAll()));
  QWebEngineProfile::defaultProfile()->scripts()->insert(s);
}

void AdBlockManager::checkForUpdates() {
  if (!cfg_.download || inFlight_ > 0 || cfg_.cacheDir.isEmpty())
    return;
  if (jobRunning_ || !queued_.isEmpty()) {
    // meta_ is about to change; judging staleness now would re-download lists
    // we have only just fetched.
    refreshTimer_.start(2000);
    return;
  }
  failures_ = 0;

  const qint64 now = QDateTime::currentSecsSinceEpoch();
  QStringList due;
  const QStringList sources = cfg_.sources();
  for (const QString &url : sources) {
    const auto it = meta_.constFind(url);
    if (it == meta_.constEnd() || now >= it->fetched + it->ttl)
      due << url;
  }
  if (due.isEmpty()) {
    scheduleNextCheck();
    return;
  }

  if (!nam_) {
    nam_ = new QNetworkAccessManager(this);
    nam_->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
  }
  for (const QString &url : std::as_const(due))
    fetch(url);
}

void AdBlockManager::fetch(const QString &url) {
  QNetworkRequest req{QUrl(url)};
  req.setRawHeader("User-Agent", "framebuffer-browser");
  req.setTransferTimeout(kTransferTimeoutMs);
  // Conditional GET: an unchanged list costs one tiny 304 instead of megabytes.
  const auto it = meta_.constFind(url);
  if (it != meta_.constEnd()) {
    if (!it->etag.isEmpty())
      req.setRawHeader("If-None-Match", it->etag.toUtf8());
    if (!it->lastModified.isEmpty())
      req.setRawHeader("If-Modified-Since", it->lastModified.toUtf8());
  }

  ++inFlight_;
  QNetworkReply *reply = nam_->get(req);
  connect(reply, &QNetworkReply::downloadProgress, reply,
          [reply](qint64 received, qint64) {
            if (received > kMaxListBytes)
              reply->abort();
          });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, url] { onReply(reply, url); });
}

void AdBlockManager::onReply(QNetworkReply *reply, const QString &url) {
  reply->deleteLater();
  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  Fetched f;
  f.url = url;
  f.fetchedAt = QDateTime::currentSecsSinceEpoch();
  bool ok = false;
  if (status == 304 && meta_.contains(url)) {
    f.notModified = true;
    ok = true;
  } else if (status == 200 && reply->error() == QNetworkReply::NoError) {
    f.body = reply->readAll();
    f.etag = QString::fromLatin1(reply->rawHeader("ETag"));
    f.lastModified = QString::fromLatin1(reply->rawHeader("Last-Modified"));
    ok = looksLikeFilterList(f.body);
  }

  if (ok) {
    batch_.append(std::move(f));
  } else {
    ++failures_;
    qWarning() << "adblock: update of" << url << "failed:"
               << (status ? QString::number(status) : reply->errorString());
  }

  if (--inFlight_ == 0)
    finishCycle();
}

void AdBlockManager::finishCycle() {
  if (!batch_.isEmpty())
    startJob(std::exchange(batch_, {})); // write files, rebuild, swap
  scheduleNextCheck();
}

void AdBlockManager::scheduleNextCheck() {
  if (failures_ > 0) { // offline, DNS not up yet, server hiccup back off
    refreshTimer_.start(retryMs_);
    retryMs_ = std::min(retryMs_ * 2, kMaxRetryMs);
  } else {
    retryMs_ = kFirstRetryMs;
    refreshTimer_.start(kCheckIntervalMs);
  }
}
