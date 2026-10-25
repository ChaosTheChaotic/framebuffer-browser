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

namespace {
// How long first nav may be held whilst filter lists loaded
constexpr int kAdblockStartupGraceMs = 5000;
} // namespace

void MainWindow::closeEvent(QCloseEvent *) {
  // This will be called whenever this window is closed.
  writeSettings();
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

void MainWindow::writeSettings() {
  QWebEngineView *view = currentWebView();
  if (!view)
    return;
  QString site = view->url().toString();
  appSettings->setValue("site", site);
  qDebug() << " write settings:" << site;
}

void MainWindow::fullScreenRequested(QWebEngineFullScreenRequest request) {
  // fullscreen on video players

  if (request.toggleOn() && !this->isFullScreen()) {
    this->showFullScreen();
  } else {
    this->showNormal();
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
void MainWindow::slotShortcutCtrlQ() {
  qDebug() << "closing1";
  // writeSettings();
  QApplication::exit();
}

// Slot handler for Ctrl + Left
void MainWindow::slotShortcutBack() {
  if (QWebEngineView *view = currentWebView()) {
    qDebug() << "Going back..";
    view->page()->triggerAction(QWebEnginePage::Back);
  }
}

// Slot handler for Ctrl + Right
void MainWindow::slotShortcutForward() {
  if (QWebEngineView *view = currentWebView()) {
    qDebug() << "Going forward..";
    view->page()->triggerAction(QWebEnginePage::Forward);
  }
}

// Slot handler for Ctrl + W
void MainWindow::slotShortcutCloseTab(int idx) {
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

QWebEngineView *MainWindow::currentWebView() const {
  return qobject_cast<QWebEngineView *>(stackedWidget->currentWidget());
}

void MainWindow::addNewTab(const QString &url) {
  QWebEngineView *webView = new QWebEngineView(stackedWidget);
  configureWebView(webView);

  webView->page()->profile()->setHttpUserAgent(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36");

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
  // Ad blocking defaults. These must be set before the early returns below so
  // a missing or broken config.json still gets a working, default setup.
  adblockCfg.download = true;
  adblockCfg.lists = AdBlockConfig::defaultLists();
  adblockCfg.cacheDir = AdBlockConfig::defaultCacheDir();
  adblockCfg.resourcesUrl = AdBlockConfig::defaultResourcesUrl();
  adblockCfg.scriptletHosts = AdBlockConfig::defaultScriptletHosts();

  bool haveUrlInCmdLine = false;
  // Check for url in argv
  if (qApp->arguments().size() > 1) {
    optStartupUrl = qApp->arguments()[1];
    haveUrlInCmdLine = true;
  }
  // Load config.json
  QFile configFile("config.json");
  if (!configFile.open(QIODevice::ReadOnly)) {
    qDebug() << "Could not open config.json";
    return;
  }
  QByteArray configData = configFile.readAll();
  configFile.close();
  QJsonParseError parseErr;
  QJsonDocument configJson = QJsonDocument::fromJson(configData, &parseErr);
  if (parseErr.error != QJsonParseError::NoError) {
    qDebug() << "JSON parse error" << parseErr.errorString();
    return;
  }
  QJsonObject configObject = configJson.object();
  // Load url
  if (configObject.contains("url") && (!haveUrlInCmdLine)) {
    if (!configObject["url"].isString()) {
      qDebug() << "Expected url to be a string, ignoring";
    } else {
      optStartupUrl = configObject["url"].toString();
    }
  }

  // Load window size
  if (configObject.contains("width")) {
    if (!configObject["width"].isDouble()) {
      qDebug() << "Expected width to be an int. Ignoring";
    } else {
      optWidth = configObject["width"].toInt();
    }
  }
  if (configObject.contains("height")) {
    if (!configObject["height"].isDouble()) {
      qDebug() << "Expected height to be an int. Ignoring";
    } else {
      optHeight = configObject["height"].toInt();
    }
  }
  // Load proxy settings
  if (configObject.contains("proxyHost")) {
    if (!configObject["proxyHost"].isString()) {
      qDebug() << "Expected proxyHost to be a string. Ignoring";
    } else {
      optProxyHost = configObject["proxyHost"].toString();
    }
  }
  if (configObject.contains("proxyPort")) {
    if (!configObject["proxyPort"].isDouble()) {
      qDebug() << "Expected proxyPort to be an int. Ignoring";
    } else {
      optProxyPort = configObject["proxyPort"].toInt();
    }
  }

  if (configObject.contains("adblock")) {
    if (!configObject["adblock"].isBool()) {
      qDebug() << "Expected adblock to be a bool. Ignoring";
    } else {
      adblock = configObject["adblock"].toBool();
    }
  }
  if (configObject.contains("adblockDownload")) {
    if (!configObject["adblockDownload"].isBool()) {
      qDebug() << "Expected adblockDownload to be a bool. Ignoring";
    } else {
      adblockCfg.download = configObject["adblockDownload"].toBool();
    }
  }
  if (configObject.contains("adblockTtlHours")) {
    if (!configObject["adblockTtlHours"].isDouble()) {
      qDebug() << "Expected adblockTtlHours to be an int. Ignoring";
    } else {
      adblockCfg.ttlHours = qMax(1, configObject["adblockTtlHours"].toInt());
    }
  }
  if (configObject.contains("adblockResources")) {
    if (!configObject["adblockResources"].isString()) {
      qDebug() << "Expected adblockResources to be a string. Ignoring";
    } else {
      const QString u = configObject["adblockResources"].toString();
      if (u.isEmpty() || u.startsWith("https://") || u.startsWith("http://")) {
        adblockCfg.resourcesUrl = u;
      } else {
        qDebug() << "Ignoring adblockResources that is not an http(s) URL:" << u;
      }
    }
  }
  if (configObject.contains("adblockScriptletHosts")) {
    if (!configObject["adblockScriptletHosts"].isArray()) {
      qDebug() << "adblockScriptletHosts should be an array of hostnames. Ignoring";
    } else {
      QStringList hosts;
      for (const QJsonValue &v : configObject["adblockScriptletHosts"].toArray()) {
        const QString h = v.toString().trimmed().toLower();
        if (!h.isEmpty()) {
          hosts << h; // validated again where the script is built
        }
      }
      adblockCfg.scriptletHosts = hosts;
    }
  }
  if (configObject.contains("adblockLists")) {
    if (!configObject["adblockLists"].isArray()) {
      qDebug() << "adblockLists should be an array of URLs. Ignoring";
    } else {
      QStringList urls;
      for (const QJsonValue &v : configObject["adblockLists"].toArray()) {
        const QString u = v.toString();
        if (u.startsWith("https://") || u.startsWith("http://")) {
          urls << u;
        } else {
          qDebug() << "Ignoring adblock list that is not an http(s) URL:" << u;
        }
      }
      adblockCfg.lists = urls; // may be empty: local folders only
    }
  }
  if (configObject.contains("adblockDirs")) {
    if (!configObject["adblockDirs"].isArray()) {
      qDebug() << "adblockDirs should be an array of paths. Ignoring";
    } else {
      for (const QJsonValue &v : configObject["adblockDirs"].toArray()) {
        if (!v.isString()) {
          qDebug() << "Ignoring non-string adblock directory";
          continue;
        }
        QString p = v.toString();
        if (p == "~" || p.startsWith("~/")) {
          p = QDir::homePath() + p.mid(1);
        }
        // Absolute now, while the working directory is still the config dir.
        p = QDir(p).absolutePath();
        if (!QDir(p).exists()) {
          qWarning() << "adblock directory does not exist (yet):" << p;
        }
        adblockCfg.localDirs << p; // kept even if missing; skipped at load
      }
    }
  }
}
