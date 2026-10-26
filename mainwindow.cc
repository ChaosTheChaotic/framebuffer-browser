#include "mainwindow.hh"
#include "adblock.hh"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QProcess>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <qkeysequence.h>
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
inline void readOpt(const QJsonObject &o, const char *key, T &out) {
  const auto it = o.constFind(QLatin1String(key));
  if (it == o.constEnd())
    return;
  using C = JConv<T>;
  if (!C::ok(*it)) {
    qDebug() << "Expected" << key << "to be" << C::what << ". Ignoring";
    return;
  }
  out = C::get(*it);
}

template <typename Filter>
inline void readStringArray(const QJsonObject &o, const char *key,
                            QStringList &out, const Filter &filter, const char *what) {
  const auto it = o.constFind(QLatin1String(key));
  if (it == o.constEnd())
    return;
  if (!it->isArray()) {
    qDebug() << key << "should be an array of" << what << ". Ignoring";
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
} // namespace

void MainWindow::closeEvent(QCloseEvent *) {
  // This will be called whenever this window is closed.
  // writeSettings();
  qDebug() << "closing2";
}

void MainWindow::configureWebView(QWebEngineView *view) {
  // Enable basic JavaScript support
  view->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
  view->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);
  view->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
  // Don't allow JavaScript to open/close windows
  view->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                                 false);
  // Allow JavaScript to access clipboard
  view->settings()->setAttribute(
      QWebEngineSettings::JavascriptCanAccessClipboard, true);
  // Allow universal access from file URLs
  view->settings()->setAttribute(
      QWebEngineSettings::LocalContentCanAccessFileUrls, true);
  view->settings()->setAttribute(
      QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
  // Enable full screen request
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

void MainWindow::addShortcut(const QKeySequence &keySeq,
                             std::function<void()> handler) {
  if (shortcuts.contains(keySeq)) {
    delete shortcuts[keySeq];
  }
  QShortcut *shortcut = new QShortcut(keySeq, this);
  connect(shortcut, &QShortcut::activated, this, [handler]() { handler(); });

  shortcuts[keySeq] = shortcut;
}

// Slot handler for Ctrl + Q
inline void MainWindow::slotShortcutCtrlQ() {
  qDebug() << "closing1";
  // writeSettings();
  QApplication::exit();
}

// Slot handler for Ctrl + Left
inline void MainWindow::slotShortcutBack() {
  if (QWebEngineView *view = currentWebView()) {
    qDebug() << "Going back..";
    view->page()->triggerAction(QWebEnginePage::Back);
  }
}

// Slot handler for Ctrl + Right
inline void MainWindow::slotShortcutForward() {
  if (QWebEngineView *view = currentWebView()) {
    qDebug() << "Going forward..";
    view->page()->triggerAction(QWebEnginePage::Forward);
  }
}

// Slot handler for Ctrl + W
inline void MainWindow::slotShortcutCloseTab(int idx) {
  if (idx < 0) {
    idx = tabBar->currentIndex();
  }
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

inline void MainWindow::slotShortcutReload() {
  if (QWebEngineView *view = currentWebView()) {
    qDebug() << "Reloading...";
    view->reload();
  }
}

inline void MainWindow::slotShortcutExitFullscreen() {
  if (QWebEngineView *view = currentWebView()) {
    qDebug() << "Exiting fullscreen...";
    view->page()->triggerAction(QWebEnginePage::ExitFullScreen);
  }
}

QWebEngineView *MainWindow::currentWebView() const {
  return qobject_cast<QWebEngineView *>(stackedWidget->currentWidget());
}

void MainWindow::addNewTab(const QString &url) {
  QWebEngineView *webView = new QWebEngineView(stackedWidget);
  configureWebView(webView);

  webView->page()->profile()->setHttpUserAgent(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/140.0.0.0 Safari/537.36");

  connect(webView->page(), &QWebEnginePage::fullScreenRequested, this,
          &MainWindow::fullScreenRequested);
  connect(webView, SIGNAL(loadStarted()), this, SLOT(onLoadStarted()));

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

  // Load proxy settings
  if (optProxyHost != "") {
    qDebug() << "Setting proxy to" << optProxyHost << ":" << optProxyPort;
    QNetworkProxy proxy;
    proxy.setType(QNetworkProxy::HttpProxy);
    proxy.setHostName(optProxyHost);
    proxy.setPort(optProxyPort);
    QNetworkProxy::setApplicationProxy(proxy);
  }

  // Set window size
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
    // Return focus back to the active web view
    if (QWebEngineView *view = currentWebView()) {
      view->setFocus();
    }
  });

  stackedWidget = new QStackedWidget(this);

  // Create a tab
  tabBar = new QTabBar(this);
  tabBar->setTabsClosable(true);

  // Add the web view to our window
  layout->addWidget(tabBar);
  layout->addWidget(urlBar);
  layout->addWidget(stackedWidget);
  setCentralWidget(mainContainer);

  connect(tabBar, &QTabBar::currentChanged, this, [this](int index) {
    if (QWebEngineView *view = currentWebView()) {
      urlBar->setText(view->url().toString());
    } else {
      urlBar->clear();
    }
  });

  // connect handler for fullscreen press on video
  connect(tabBar, &QTabBar::tabCloseRequested, this,
          &MainWindow::slotShortcutCloseTab);

  // Default shortcuts
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q),
              [this]() { slotShortcutCtrlQ(); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left),
              [this]() { slotShortcutBack(); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right),
              [this]() { slotShortcutForward(); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_T),
              [this]() { addNewTab(optStartupUrl); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_W),
              [this]() { slotShortcutCloseTab(); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), [this]() {
    urlBar->setFocus();
    urlBar->selectAll();
  });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_R),
              [this]() { slotShortcutReload(); });
  addShortcut(QKeySequence(Qt::Key_Escape),
              [this]() { slotShortcutExitFullscreen(); });

  if (adblock) {
    adblockMgr = new AdBlockManager(adblockCfg, this);
    QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(
        adblockMgr->interceptor());
    // Hold the first nav until cached lists are in so first page is filtered,
    // but never longer than grace Whichever fires first wins
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
  // Detach so Qt cant call interceptor then let manager go
  // This might wait as it needs a list build that is still running
  QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(nullptr);
  delete adblockMgr;
  adblockMgr = nullptr;
  qDebug() << "main window destroyed";
  destroy();
}

void MainWindow::onLoadStarted() { qDebug() << "onLoadStarted"; }

void MainWindow::loadUrl(QString url) {
  if (stackedWidget->count() == 0) {
    addNewTab(url);
  } else {
    QWebEngineView *view = currentWebView();
    if (url.startsWith("./")) {
      url = "file:///" + QDir::currentPath() + QDir::separator() + url.mid(2);
    }
    qDebug() << "Loading URL in current tab:" << url;
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
  // Defaults first, so a missing or broken config.json still works.
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
    qDebug() << "Could not open config.json";
    return;
  }
  QJsonParseError parseErr;
  const QJsonObject o =
      QJsonDocument::fromJson(configFile.readAll(), &parseErr).object();
  if (parseErr.error != QJsonParseError::NoError) {
    qDebug() << "JSON parse error" << parseErr.errorString();
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
      qDebug() << "Expected adblockTtlHours to be an int. Ignoring";
  }

  if (auto it = o.constFind(QLatin1String("adblockResources"));
      it != o.constEnd()) {
    if (!it->isString()) {
      qDebug() << "Expected adblockResources to be a string. Ignoring";
    } else {
      const QString u = it->toString();
      if (u.isEmpty() || u.startsWith("https://") || u.startsWith("http://"))
        adblockCfg.resourcesUrl = u;
      else
        qDebug() << "Ignoring adblockResources that is not an http(s) URL:"
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
        qDebug() << "Ignoring adblock list that is not an http(s) URL:" << s;
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
