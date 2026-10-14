#include "mainwindow.hh"
#include "qboxlayout.h"
#include "qnamespace.h"
#include "qobject.h"
#include "qobjectdefs.h"
#include "qtabwidget.h"
#include "qurl.h"
#include "qwebengineview.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QProcess>
#include <QTimer>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <qkeysequence.h>

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
      "Mozilla/5.0 (X11; Linux x86_64; rv:89.0) Gecko/20100101 Firefox/89.0");

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

  // Ctrl + Q
  keyCtrlQ = new QShortcut(this);                       // Initialize the object
  keyCtrlQ->setKey(QKeySequence(Qt::CTRL | Qt::Key_Q)); // Set the key code
  // connect handler to keypress
  connect(keyCtrlQ, SIGNAL(activated()), this, SLOT(slotShortcutCtrlQ()));

  keyBack = new QShortcut(this);
  keyBack->setKey(QKeySequence(Qt::CTRL | Qt::Key_Left));
  connect(keyBack, SIGNAL(activated()), this, SLOT(slotShortcutBack()));

  keyForward = new QShortcut(this);
  keyForward->setKey(QKeySequence(Qt::CTRL | Qt::Key_Right));
  connect(keyForward, SIGNAL(activated()), this, SLOT(slotShortcutForward()));

  keyCtrlT = new QShortcut(this);
  keyCtrlT->setKey(QKeySequence(Qt::CTRL | Qt::Key_T));
  connect(keyCtrlT, &QShortcut::activated, this,
          [this]() { addNewTab(optStartupUrl); });

  keyCtrlW = new QShortcut(this);
  keyCtrlW->setKey(QKeySequence(Qt::CTRL | Qt::Key_W));
  connect(keyCtrlW, &QShortcut::activated, this,
          [this]() { slotShortcutCloseTab(); });

  keyCtrlL = new QShortcut(this);
  keyCtrlL->setKey(QKeySequence(Qt::CTRL | Qt::Key_L));
  connect(keyCtrlL, &QShortcut::activated, this, [this]() {
    urlBar->setFocus();
    urlBar->selectAll();
  });

  QTimer::singleShot(0, this, SLOT(loadStartupUrl()));
}

MainWindow::~MainWindow() {
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

void MainWindow::loadStartupUrl() { loadUrl(optStartupUrl); }

void MainWindow::loadConfig() {
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
  QJsonDocument configJson = QJsonDocument::fromJson(configData);
  QJsonObject configObject = configJson.object();
  // Load url
  if (configObject.contains("url") && (!haveUrlInCmdLine)) {
    optStartupUrl = configObject["url"].toString();
  }

  // Load window size
  if (configObject.contains("width")) {
    optWidth = configObject["width"].toInt();
  }
  if (configObject.contains("height")) {
    optHeight = configObject["height"].toInt();
  }
  // Load proxy settings
  if (configObject.contains("proxyHost")) {
    optProxyHost = configObject["proxyHost"].toString();
  }
  if (configObject.contains("proxyPort")) {
    optProxyPort = configObject["proxyPort"].toInt();
  }
}
