#ifndef BROWSERWINDOW_H
#define BROWSERWINDOW_H

#include "adblock.hh"
#include <QCloseEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QWebEngineFullScreenRequest>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWidget>
#include <qwebengineview.h>

class MainWindow : public QMainWindow {
  Q_OBJECT

  // Define signals and slots
  QString optStartupUrl;
  int optWidth = 0;
  int optHeight = 0;
  QString optProxyHost = "";
  int optProxyPort = 0;
  bool adblock = true;       // master switch ("adblock" in config.json)
  AdBlockConfig adblockCfg;  // download toggle, lists, local dirs, cache dir

public:
  QTabBar *tabBar;
  QStackedWidget *stackedWidget;
  QLineEdit *urlBar;
  explicit MainWindow();
  ~MainWindow();
  void configureWebView(QWebEngineView *view);
  void loadConfig();
  void loadUrl(QString url);
  QWebEngineView *currentWebView() const;

public slots:
  void loadStartupUrl();
  void onLoadStarted();
  void slotShortcutCtrlQ();
  void slotShortcutBack();
  void slotShortcutForward();
  void slotShortcutCloseTab(int idx = -1);
  void addNewTab(const QString &url = QString());

private:
  QHash<QKeySequence, QShortcut *> shortcuts;
  void addShortcut(const QKeySequence &keySeq, std::function<void()> handler);
  void fullScreenRequested(QWebEngineFullScreenRequest request);
  void writeSettings();
  QSettings *appSettings;
  AdBlockManager *adblockMgr = nullptr;
  bool startupLoaded = false;

protected:
  void closeEvent(QCloseEvent *) override;
};

#endif // BROWSERWINDOW_H
