#ifndef BROWSERWINDOW_H
#define BROWSERWINDOW_H

#include "adblock.hh"
#include <QByteArray>
#include <QCloseEvent>
#include <QHash>
#include <QLineEdit>
#include <QMainWindow>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QWebEngineFullScreenRequest>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWidget>

class MainWindow : public QMainWindow {
  Q_OBJECT

  // Configuration
  QString optStartupUrl;
  int optWidth = 0;
  int optHeight = 0;
  QString optProxyHost = "";
  int optProxyPort = 0;
  bool adblock = true;
  AdBlockConfig adblockCfg;

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

  bool setShortcut(const QByteArray &name, const QKeySequence &seq);
  QKeySequence shortcutFor(const QByteArray &name) const;

public slots:
  void loadStartupUrl();
  void slotShortcutCtrlQ();
  void slotShortcutBack();
  void slotShortcutForward();
  void slotShortcutCloseTab(int idx = -1);
  void slotShortcutCloseCurrentTab(); // no-arg wrapper for table
  void slotShortcutNewTab();
  void slotShortcutFocusUrl();
  void slotShortcutReload();
  void slotShortcutExitFullscreen();
  void addNewTab(const QString &url = QString());

private:
  struct ShortcutBinding {
    QKeySequence seq;
    void (MainWindow::*slot)() = nullptr;
    QShortcut *shortcut = nullptr;
  };

  QHash<QByteArray, ShortcutBinding>
      bindings;                              // name -> {seq, slot, QShortcut*}
  QHash<QKeySequence, QByteArray> seqToName; // seq  -> name (conflict map)

  void installShortcuts();
  void fullScreenRequested(QWebEngineFullScreenRequest request);
  void writeSettings();
  QSettings *appSettings;
  AdBlockManager *adblockMgr = nullptr;
  bool startupLoaded = false;
  Qt::WindowStates prevState = Qt::WindowNoState;

protected:
  // void closeEvent(QCloseEvent *) override;
};

#endif // BROWSERWINDOW_H
