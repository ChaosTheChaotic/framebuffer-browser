#include "mainwindow.hh"
#include "adblock.hh"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QNetworkProxy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <optional>
#include <utility>

namespace {
template <typename T> struct JConv;

template <> struct JConv<int> {
  static bool ok(const QJsonValue &v) { return v.isDouble(); }
  static int get(const QJsonValue &v) { return v.toInt(); }
  static constexpr const char *what = "an int";
};
template <> struct JConv<bool> {
  static bool ok(const QJsonValue &v) { return v.isBool(); }
  static bool get(const QJsonValue &v) { return v.toBool(); }
  static constexpr const char *what = "a bool";
};
template <> struct JConv<QString> {
  static bool ok(const QJsonValue &v) { return v.isString(); }
  static QString get(const QJsonValue &v) { return v.toString(); }
  static constexpr const char *what = "a string";
};

// Assign if key exists and has right type, otherwise print
template <typename T>
void readOpt(const QJsonObject &o, const char *key, T &out) {
  const auto it = o.constFind(QLatin1String(key));
  if (it == o.constEnd())
    return;
  using C = JConv<T>;
  if (!C::ok(*it)) {
    qWarning() << "Expected" << key << "to be" << C::what << ". Ignoring";
    return;
  }
  out = C::get(*it);
}

template <typename Filter>
void readStringArray(const QJsonObject &o, const char *key, QStringList &out,
                     const Filter &filter, const char *what) {
  const auto it = o.constFind(QLatin1String(key));
  if (it == o.constEnd())
    return;
  if (!it->isArray()) {
    qWarning() << key << "should be an array of" << what << ". Ignoring";
    return;
  }
  QStringList result;
  for (const QJsonValue &v : it->toArray())
    if (v.isString())
      if (auto s = filter(v.toString()))
        result << std::move(*s);
  out = std::move(result);
}

} // namespace

namespace {
// How long first nav may be held whilst filter lists loaded
constexpr int kAdblockStartupGraceMs = 5000;
struct ShortcutDef {
  const char *name;
  QKeySequence seq;
  void (MainWindow::*slot)();
};

} // namespace

// void MainWindow::closeEvent(QCloseEvent *) {
//   // writeSettings();
// }

void MainWindow::configureWebView(QWebEngineView *view) {
  view->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
  view->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);
  view->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
  view->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                                 false);
  view->settings()->setAttribute(
      QWebEngineSettings::JavascriptCanAccessClipboard, true);
  view->settings()->setAttribute(
      QWebEngineSettings::LocalContentCanAccessFileUrls, true);
  view->settings()->setAttribute(
      QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
  view->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled,
                                 true);
}

// void MainWindow::writeSettings() {
//   QWebEngineView *view = currentWebView();
//   if (!view)
//     return;
//   QString site = view->url().toString();
//   appSettings->setValue("site", site);
//   qDebug() << " write settings:" << site;
// }

void MainWindow::fullScreenRequested(QWebEngineFullScreenRequest request) {
  if (request.toggleOn()) {
    prevState = windowState();
    tabBar->hide();
    urlBar->hide();
    this->showFullScreen();
  } else {
    tabBar->show();
    urlBar->show();
    this->setWindowState(prevState);
  }
  request.accept();
}

void MainWindow::installShortcuts() {
  const ShortcutDef kShortcutTable[] = {
      {"quit", Qt::CTRL | Qt::Key_Q, &MainWindow::slotShortcutCtrlQ},
      {"back", Qt::CTRL | Qt::Key_Left, &MainWindow::slotShortcutBack},
      {"forward", Qt::CTRL | Qt::Key_Right, &MainWindow::slotShortcutForward},
      {"newTab", Qt::CTRL | Qt::Key_T, &MainWindow::slotShortcutNewTab},
      {"closeTab", Qt::CTRL | Qt::Key_W,
       &MainWindow::slotShortcutCloseCurrentTab},
      {"focusUrl", Qt::CTRL | Qt::Key_L, &MainWindow::slotShortcutFocusUrl},
      {"reload", Qt::CTRL | Qt::Key_R, &MainWindow::slotShortcutReload},
      {"exitFull", Qt::Key_Escape, &MainWindow::slotShortcutExitFullscreen},
  };

  for (const auto &def : kShortcutTable) {
    const QByteArray name(def.name);
    ShortcutBinding &b = bindings[name];
    b.seq = def.seq;
    b.slot = def.slot;
    b.shortcut = new QShortcut(b.seq, this);
    connect(b.shortcut, &QShortcut::activated, this, b.slot);
    seqToName.insert(b.seq, name);
  }
}

// Rebind a named action at runtime
bool MainWindow::setShortcut(const QByteArray &name, const QKeySequence &seq) {
  auto it = bindings.find(name);
  if (it == bindings.end())
    return false;

  // Drop this action's previous binding.
  if (it->shortcut) {
    it->shortcut->deleteLater();
    it->shortcut = nullptr;
  }
  if (!it->seq.isEmpty())
    seqToName.remove(it->seq);

  // Take ownership of seq
  if (auto owner = seqToName.constFind(seq); owner != seqToName.constEnd()) {
    auto obIt = bindings.find(*owner);
    if (obIt != bindings.end()) {
      if (obIt->shortcut) {
        obIt->shortcut->deleteLater();
        obIt->shortcut = nullptr;
      }
      obIt->seq = QKeySequence();
    }
    seqToName.remove(seq);
  }

  it->seq = seq;
  if (!seq.isEmpty()) {
    it->shortcut = new QShortcut(seq, this);
    connect(it->shortcut, &QShortcut::activated, this, it->slot);
    seqToName.insert(seq, name);
  }
  return true;
}

QKeySequence MainWindow::shortcutFor(const QByteArray &name) const {
  auto it = bindings.constFind(name);
  return it == bindings.constEnd() ? QKeySequence() : it->seq;
}

void MainWindow::slotShortcutCtrlQ() {
  // writeSettings();
  QApplication::exit();
}

void MainWindow::slotShortcutBack() {
  if (QWebEngineView *view = currentWebView())
    view->page()->triggerAction(QWebEnginePage::Back);
}

void MainWindow::slotShortcutForward() {
  if (QWebEngineView *view = currentWebView())
    view->page()->triggerAction(QWebEnginePage::Forward);
}

void MainWindow::slotShortcutCloseTab(int idx) {
  if (idx < 0)
    idx = tabBar->currentIndex();
  if (idx >= 0 && idx < stackedWidget->count()) {
    if (stackedWidget->count() > 1) {
      QWidget *widget = stackedWidget->widget(idx);
      tabBar->removeTab(idx);
      widget->deleteLater();
    } else {
      close();
    }
  }
}

void MainWindow::slotShortcutCloseCurrentTab() { slotShortcutCloseTab(-1); }

void MainWindow::slotShortcutNewTab() { addNewTab(optStartupUrl); }

void MainWindow::slotShortcutFocusUrl() {
  urlBar->setFocus();
  urlBar->selectAll();
}

void MainWindow::slotShortcutReload() {
  if (QWebEngineView *view = currentWebView())
    view->reload();
}

void MainWindow::slotShortcutExitFullscreen() {
  if (QWebEngineView *view = currentWebView())
    view->page()->triggerAction(QWebEnginePage::ExitFullScreen);
}

QWebEngineView *MainWindow::currentWebView() const {
  return qobject_cast<QWebEngineView *>(stackedWidget->currentWidget());
}

void MainWindow::addNewTab(const QString &url) {
  QWebEngineView *webView = new QWebEngineView(stackedWidget);
  configureWebView(webView);

  connect(webView->page(), &QWebEnginePage::fullScreenRequested, this,
          &MainWindow::fullScreenRequested);

  int idx = stackedWidget->addWidget(webView);
  tabBar->addTab("New Tab");
  tabBar->setCurrentIndex(idx);
  stackedWidget->setCurrentIndex(idx);

  connect(webView, &QWebEngineView::titleChanged, this,
          [this, webView](const QString &title) {
            int idx = stackedWidget->indexOf(webView);
            if (idx != -1) {
              tabBar->setTabText(idx, title.left(20));
            }
          });

  connect(webView, &QWebEngineView::urlChanged, this,
          [this, webView](const QUrl &newUrl) {
            if (webView == currentWebView()) {
              urlBar->setText(newUrl.toString());
            }
          });

  if (!url.isEmpty()) {
    QString target = url;
    if (target.startsWith("./")) {
      target =
          "file:///" + QDir::currentPath() + QDir::separator() + target.mid(2);
    }
    webView->load(QUrl(target));
  }
}

MainWindow::MainWindow() : QMainWindow() {
  loadConfig();

  appSettings = new QSettings(this);

  if (!optProxyHost.isEmpty()) {
    qDebug() << "Setting proxy to" << optProxyHost << ":" << optProxyPort;
    QNetworkProxy proxy;
    proxy.setType(QNetworkProxy::HttpProxy);
    proxy.setHostName(optProxyHost);
    proxy.setPort(optProxyPort);
    QNetworkProxy::setApplicationProxy(proxy);
  }

  if (optWidth) {
    resize(optWidth, optHeight);
  }

  QWidget *mainContainer = new QWidget(this);
  QVBoxLayout *layout = new QVBoxLayout(mainContainer);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  urlBar = new QLineEdit(this);
  urlBar->setPlaceholderText("Enter URL");

  connect(urlBar, &QLineEdit::returnPressed, this, [this]() {
    QString text = urlBar->text();
    if (!text.isEmpty()) {
      loadUrl(text);
    }
    if (QWebEngineView *view = currentWebView()) {
      view->setFocus();
    }
  });

  stackedWidget = new QStackedWidget(this);

  tabBar = new QTabBar(this);
  tabBar->setTabsClosable(true);

  layout->addWidget(tabBar);
  layout->addWidget(urlBar);
  layout->addWidget(stackedWidget);
  setCentralWidget(mainContainer);

  connect(tabBar, &QTabBar::currentChanged, this, [this](int) {
    if (QWebEngineView *view = currentWebView()) {
      urlBar->setText(view->url().toString());
    } else {
      urlBar->clear();
    }
  });

  connect(tabBar, &QTabBar::tabCloseRequested, this,
          &MainWindow::slotShortcutCloseTab);

  installShortcuts();

  QWebEngineProfile::defaultProfile()->setHttpUserAgent(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/140.0.0.0 Safari/537.36");

  if (adblock) {
    adblockMgr = new AdBlockManager(adblockCfg, this);
    QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(
        adblockMgr->interceptor());
    connect(adblockMgr, &AdBlockManager::settled, this,
            &MainWindow::loadStartupUrl);
    QTimer::singleShot(kAdblockStartupGraceMs, this,
                       &MainWindow::loadStartupUrl);
    adblockMgr->start();
  } else {
    QTimer::singleShot(0, this, &MainWindow::loadStartupUrl);
  }
}

MainWindow::~MainWindow() {
  QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(nullptr);
}

void MainWindow::loadUrl(QString url) {
  if (stackedWidget->count() == 0) {
    addNewTab(url);
  } else {
    QWebEngineView *view = currentWebView();
    if (url.startsWith("./")) {
      url = "file:///" + QDir::currentPath() + QDir::separator() + url.mid(2);
    }
    view->load(QUrl(url));
  }
}

void MainWindow::loadStartupUrl() {
  if (startupLoaded)
    return;
  startupLoaded = true;
  loadUrl(optStartupUrl);
}

void MainWindow::loadConfig() {
  adblockCfg.download = true;
  adblockCfg.lists = AdBlockConfig::defaultLists();
  adblockCfg.cacheDir = AdBlockConfig::defaultCacheDir();
  adblockCfg.resourcesUrl = AdBlockConfig::defaultResourcesUrl();
  adblockCfg.scriptletHosts = AdBlockConfig::defaultScriptletHosts();

  const bool haveUrlInCmdLine = qApp->arguments().size() > 1;
  if (haveUrlInCmdLine)
    optStartupUrl = qApp->arguments()[1];

  QFile configFile("config.json");
  if (!configFile.open(QIODevice::ReadOnly)) {
    qWarning() << "Could not open config.json";
    return;
  }
  QJsonParseError parseErr;
  const QJsonObject o =
      QJsonDocument::fromJson(configFile.readAll(), &parseErr).object();
  if (parseErr.error != QJsonParseError::NoError) {
    qWarning() << "JSON parse error" << parseErr.errorString();
    return;
  }

  if (!haveUrlInCmdLine)
    readOpt(o, "url", optStartupUrl);
  readOpt(o, "width", optWidth);
  readOpt(o, "height", optHeight);
  readOpt(o, "proxyHost", optProxyHost);
  readOpt(o, "proxyPort", optProxyPort);
  readOpt(o, "adblock", adblock);
  readOpt(o, "adblockDownload", adblockCfg.download);

  if (auto it = o.constFind(QLatin1String("adblockTtlHours"));
      it != o.constEnd()) {
    if (it->isDouble())
      adblockCfg.ttlHours = qMax(1, it->toInt());
    else
      qWarning() << "Expected adblockTtlHours to be an int. Ignoring";
  }

  if (auto it = o.constFind(QLatin1String("adblockResources"));
      it != o.constEnd()) {
    if (!it->isString()) {
      qWarning() << "Expected adblockResources to be a string. Ignoring";
    } else {
      const QString u = it->toString();
      if (u.isEmpty() || u.startsWith("https://") || u.startsWith("http://"))
        adblockCfg.resourcesUrl = u;
      else
        qWarning() << "Ignoring adblockResources that is not an http(s) URL:"
                   << u;
    }
  }

  readStringArray(
      o, "adblockScriptletHosts", adblockCfg.scriptletHosts,
      [](QString s) -> std::optional<QString> {
        s = s.trimmed().toLower();
        if (s.isEmpty())
          return std::nullopt;
        return std::optional<QString>(std::move(s));
      },
      "hostnames");

  readStringArray(
      o, "adblockLists", adblockCfg.lists,
      [](const QString &s) -> std::optional<QString> {
        if (s.startsWith("https://") || s.startsWith("http://"))
          return s;
        qWarning() << "Ignoring adblock list that is not an http(s) URL:" << s;
        return std::nullopt;
      },
      "http(s) URLs");

  readStringArray(
      o, "adblockDirs", adblockCfg.localDirs,
      [](QString p) -> std::optional<QString> {
        if (p == "~" || p.startsWith("~/"))
          p = QDir::homePath() + p.mid(1);
        p = QDir(p).absolutePath();
        if (!QDir(p).exists())
          qWarning() << "adblock directory does not exist (yet):" << p;
        return p;
      },
      "paths");
}
