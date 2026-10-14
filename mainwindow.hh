#ifndef BROWSERWINDOW_H
#define BROWSERWINDOW_H

#include "qtabwidget.h"
#include <QCloseEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QTabWidget>
#include <QTabBar>
#include <QStackedWidget>
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
  QShortcut *keyCtrlQ;   // Entity of Ctrl + Q hotkeys
  QShortcut *keyBack;    // back in history
  QShortcut *keyForward; // forward in history
  QShortcut *keyCtrlT;   // Create a new tab
  QShortcut *keyCtrlW;   // Close a tab
  QShortcut *keyCtrlL;   // Focus url bar
  void fullScreenRequested(QWebEngineFullScreenRequest request);
  void writeSettings();
  QSettings *appSettings;

protected:
  void closeEvent(QCloseEvent *) override;
};

#endif // BROWSERWINDOW_H
