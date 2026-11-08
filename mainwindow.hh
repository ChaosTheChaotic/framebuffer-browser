#ifndef BROWSERWINDOW_H
#define BROWSERWINDOW_H

#include "adblock.hh"
#include "omnibox.hh"
#include <QByteArray>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QHash>
#include <QLineEdit>
#include <QMainWindow>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QWebEngineFullScreenRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QWidget>
#include <memory>

class TermFileChooser;

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
  QStringList optExtensionDirs;
  QString optFileChooser = "builtin"; // "builtin" | "terminal"
  QString optChooserShell;            // terminal chooser, empty = $SHELL
  QString optChooserFont; // terminal chooser, empty = system fixed font
  int optChooserFontSize = 13;

public:
  QTabBar *tabBar;
  QStackedWidget *stackedWidget;
  QLineEdit *urlBar;
  explicit MainWindow();
  ~MainWindow();
  void configureWebView(QWebEngineView *view);
  void loadConfig();
  void loadUrl(const QUrl &url);
  QWebEngineView *currentWebView() const;
  // File picker of a page, answered by a shell in an embedded terminal.
  // mode is QWebEnginePage::FileSelectionMode as an int (see mainwindow.cc)
  QStringList chooseFilesInTerminal(int mode, const QStringList &oldFiles,
                                    const QStringList &accept);

  bool setShortcut(const QByteArray &name, const QKeySequence &seq);
  QKeySequence shortcutFor(const QByteArray &name) const;

public slots:
  void loadStartupUrl();
  void slotShortcutCtrlQ();
  void slotShortcutBack();
  void slotShortcutForward();
  void slotShortcutCloseTab(int idx = -1);
  void slotShortcutCloseCurrentTab(); // no arg wrapper for table
  void slotShortcutNewTab();
  void slotShortcutFocusUrl();
  void slotShortcutReload();
  void slotShortcutExitFullscreen();
  void addNewTab(const QUrl &url = QUrl());

private:
  struct ShortcutBinding {
    QKeySequence seq;
    void (MainWindow::*slot)() = nullptr;
    QShortcut *shortcut = nullptr;
  };

  QHash<QByteArray, ShortcutBinding>
      bindings;                              // name -> {seq, slot, QShortcut*}
  QHash<QKeySequence, QByteArray> seqToName; // seq  -> name (conflict map)

  QString optSearchUrl; // empty = default
  bool optTldUpdate = true;
  Omnibox *omnibox = nullptr;

  QElapsedTimer startupClock;

  void reloadStaleTabs(const std::function<bool(const QUrl &)> &pred);

  unsigned int pendingExts = 0;
  bool extsGaveUp = false;

  bool adblockSettled = false;

  int envGen = 0; // Changed when adblock scripst and extensions arrive

  void installShortcuts();
  void installVimKeys();
  void vimCommand(QWebEngineView *src, const QString &cmd);
  QString vimToken; // random per run, only known to the injected script
  void fullScreenRequested(QWebEngineFullScreenRequest request);
  void writeSettings();
  QSettings *appSettings;
  QStackedWidget *rootStack =
      nullptr; // main content, plus the file chooser terminal
  std::unique_ptr<TermFileChooser> termChooser; // null = Qts own file dialog
  QWebEngineProfile *profile = nullptr; // Persistent which extensions need
  AdBlockManager *adblockMgr = nullptr;
  bool startupLoaded = false;
  Qt::WindowStates prevState = Qt::WindowNoState;

protected:
  // void closeEvent(QCloseEvent *) override;
};

#endif // BROWSERWINDOW_H
