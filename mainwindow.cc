#include "mainwindow.hh"
#include "adblock.hh"
#include "filechooser.hh"
#include "omnibox.hh"

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QNetworkProxy>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWebEngineExtensionInfo>
#include <QWebEngineExtensionManager>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <functional>
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
constexpr int kAdblockStartupGraceMs = 500;
struct ShortcutDef {
  const char *name;
  QKeySequence seq;
  void (MainWindow::*slot)();
};
constexpr int kLateReloadWindowMs =
    8000; // dont yank pages the user has settled into

} // namespace

// void MainWindow::closeEvent(QCloseEvent *) {
//   // writeSettings();
// }

namespace {
// Receives the commands vimkeys.js sends as console.log(TOKEN + cmd). Messages
// that dont carry the token are ordinary page console output
// Also sends the pages file pickers to the terminal chooser when that is enabled
class VimPage : public QWebEnginePage {
public:
  // (FileSelectionMode as int, old files, accepted MIME types) -> chosen files
  using ChooseFn =
      std::function<QStringList(int, const QStringList &, const QStringList &)>;

  VimPage(QWebEngineProfile *profile, QString token,
          std::function<void(const QString &)> onCmd, ChooseFn onChoose,
          QObject *parent)
      : QWebEnginePage(profile, parent), token_(std::move(token)),
        onCmd_(std::move(onCmd)), onChoose_(std::move(onChoose)) {}

protected:
  QStringList chooseFiles(FileSelectionMode mode, const QStringList &oldFiles,
                          const QStringList &accept) override {
    if (onChoose_)
      return onChoose_(static_cast<int>(mode), oldFiles, accept);
    return QWebEnginePage::chooseFiles(mode, oldFiles, accept); // Qts dialog
  }

  void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                const QString &msg, int line,
                                const QString &src) override {
    if (msg.startsWith(token_)) {
      onCmd_(msg.mid(token_.size()));
      return;
    }
    QWebEnginePage::javaScriptConsoleMessage(level, msg, line, src);
  }

private:
  QString token_;
  std::function<void(const QString &)> onCmd_;
  ChooseFn onChoose_;
};
} // namespace

void MainWindow::installVimKeys() {
  QFile f(QStringLiteral(":/vimkeys/vimkeys.js"));
  if (!f.open(QIODevice::ReadOnly)) {
    qWarning() << "vimkeys: missing resource :/vimkeys/vimkeys.js";
    return;
  }
  QString src = QString::fromUtf8(f.readAll());
  src.replace(QStringLiteral("__FBB_TOKEN__"), vimToken);

  QWebEngineScript s;
  s.setName(QStringLiteral("fbb-vimkeys")); // adblock only removes "fbb-adblock:*"
  s.setInjectionPoint(QWebEngineScript::DocumentCreation);
  s.setWorldId(QWebEngineScript::ApplicationWorld); // isolated from the page
  s.setRunsOnSubFrames(false);
  s.setSourceCode(src);
  profile->scripts()->insert(s);
}

void MainWindow::vimCommand(QWebEngineView *src, const QString &cmd) {
  if (src != currentWebView())
    return; // a background tab must not drive the browser
  const int sp = cmd.indexOf(QLatin1Char(' '));
  const QString name = sp < 0 ? cmd : cmd.left(sp);
  const QString arg = sp < 0 ? QString() : cmd.mid(sp + 1);
  const int n = stackedWidget->count();

  if (name == QLatin1String("tabnext") && n > 1) {
    tabBar->setCurrentIndex((tabBar->currentIndex() + 1) % n);
  } else if (name == QLatin1String("tabprev") && n > 1) {
    tabBar->setCurrentIndex((tabBar->currentIndex() + n - 1) % n);
  } else if (name == QLatin1String("tabnew")) {
    slotShortcutNewTab();
  } else if (name == QLatin1String("tabclose")) {
    slotShortcutCloseTab(stackedWidget->indexOf(src));
  } else if (name == QLatin1String("urlfocus")) {
    slotShortcutFocusUrl();
  } else if (name == QLatin1String("open")) {
    const QUrl u(arg);
    if (u.scheme() == QLatin1String("http") ||
        u.scheme() == QLatin1String("https"))
      addNewTab(u);
  } else if (name == QLatin1String("yank")) {
    QGuiApplication::clipboard()->setText(arg);
  } else if (name == QLatin1String("paste") ||
             name == QLatin1String("pastetab")) {
    const QString text = QGuiApplication::clipboard()->text().trimmed();
    if (text.isEmpty())
      return;
    const QUrl u = omnibox->resolve(text);
    if (name == QLatin1String("pastetab"))
      addNewTab(u);
    else
      src->load(u);
  }
}

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
  // A window-wide Escape shortcut would swallow Escape before the page (and
  // vimkeys.js) sees it, so it is only live while a page is fullscreen.
  if (auto it = bindings.find("exitFull"); it != bindings.end() && it->shortcut)
    it->shortcut->setEnabled(request.toggleOn());
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
  bindings["exitFull"].shortcut->setEnabled(false); // see fullScreenRequested
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

void MainWindow::reloadStaleTabs(
    const std::function<bool(const QUrl &)> &pred) {
  if (startupClock.elapsed() > kLateReloadWindowMs)
    return; // next navigation picks the new environment up anyway
  for (int i = 0; i < stackedWidget->count(); ++i) {
    auto *v = qobject_cast<QWebEngineView *>(stackedWidget->widget(i));
    if (!v || v->property("envGen").toInt() >= envGen)
      continue;
    const QUrl u = v->page()->requestedUrl(); // url() is empty mid load
    if ((u.scheme() == "http" || u.scheme() == "https") && pred(u))
      v->reload();
  }
}

QStringList MainWindow::chooseFilesInTerminal(int mode,
                                              const QStringList &oldFiles,
                                              const QStringList &accept) {
  // QWebEnginePage::FileSelectionMode only names Open and OpenMultiple, but Qt
  // passes Chromiums other two chooser modes through the same argument (its own
  // chooseFiles() switches on them), numbered after those two
  constexpr int kOpenMultiple = QWebEnginePage::FileSelectOpenMultiple;
  constexpr int kUploadFolder = 2;
  constexpr int kSave = 3;

  if (!termChooser)
    return {};

  TermFileChooser::Request req;
  req.multiple = mode == kOpenMultiple;
  req.directory = mode == kUploadFolder;
  req.save = mode == kSave;
  if (req.save && !oldFiles.isEmpty()) // for a save the page proposes a name
    req.suggested = QFileInfo(oldFiles.first()).fileName();
  req.accept = accept;

  // The window-wide shortcuts would take keys the shell needs
	// Same idea as the Escape shortcut in fullScreenRequested
  QList<std::pair<QPointer<QShortcut>, bool>> saved;
  for (const ShortcutBinding &b : std::as_const(bindings))
    if (b.shortcut) {
      saved.emplaceBack(b.shortcut, b.shortcut->isEnabled());
      b.shortcut->setEnabled(false);
    }

  const QStringList files = termChooser->choose(req);

  for (const auto &s : saved)
    if (s.first)
      s.first->setEnabled(s.second);
  return files;
}

void MainWindow::addNewTab(const QUrl &url) {
  QWebEngineView *webView = new QWebEngineView(stackedWidget);
  VimPage::ChooseFn choose; // empty, the page opens Qts own file dialog
  if (termChooser)
    choose = [this](int mode, const QStringList &oldFiles,
                    const QStringList &accept) {
      return chooseFilesInTerminal(mode, oldFiles, accept);
    };
  webView->setPage(new VimPage(
      profile, vimToken,
      [this, webView](const QString &cmd) { vimCommand(webView, cmd); },
      std::move(choose), webView));
  configureWebView(webView);

  connect(webView, &QWebEngineView::loadStarted, this,
          [this, webView] { webView->setProperty("envGen", envGen); });

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
    webView->load(url);
  }
}

namespace {
constexpr int kExtensionStartupGraceMs = 2000;

int loadExtensions(const QString &path, QWebEngineExtensionManager *extMgr) {
  const QDir baseDir(path);
  if (QFileInfo::exists(baseDir.absoluteFilePath("manifest.json"))) {
    extMgr->loadExtension(baseDir.absolutePath());
    return 1; // Dont bother scanning inside of extensions
  }
  int n = 0;
  for (const QFileInfo &d :
       baseDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (QFileInfo::exists(
            QDir(d.absoluteFilePath()).absoluteFilePath("manifest.json"))) {
      extMgr->loadExtension(d.absoluteFilePath());
      ++n;
    }
  }
  return n;
}
} // namespace

MainWindow::MainWindow() : QMainWindow() {
  loadConfig();

  // Extensions added with fbbrowser --install-extension are here
  // The launcher passes the folder in FBBROWSER_EXTENSIONS_DIR so both agree
  {
    const QString env = qEnvironmentVariable("FBBROWSER_EXTENSIONS_DIR");
    const QString userExtDir = env.isEmpty()
                                   ? QStandardPaths::writableLocation(
                                         QStandardPaths::AppDataLocation) +
                                         QStringLiteral("/extensions")
                                   : env;
    if (!userExtDir.isEmpty() && !optExtensionDirs.contains(userExtDir))
      optExtensionDirs.prepend(userExtDir);
  }

  // Extensions dont load in OTR profile, and default is OTR
  // Profiles with storage name is persistent
  profile = new QWebEngineProfile(QStringLiteral("fbbrowser"), this);
  QWebEngineExtensionManager *extMgr = profile->extensionManager();

  appSettings = new QSettings(this);

  if (!optProxyHost.isEmpty()) {
    qDebug() << "Setting proxy to" << optProxyHost << ":" << optProxyPort;
    QNetworkProxy proxy;
    proxy.setType(QNetworkProxy::HttpProxy);
    proxy.setHostName(optProxyHost);
    proxy.setPort(optProxyPort);
    QNetworkProxy::setApplicationProxy(proxy);
  }

  omnibox = new Omnibox(optSearchUrl, optTldUpdate, this);
  omnibox->start();

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
    loadUrl(omnibox->resolve(urlBar->text()));
    if (QWebEngineView *view = currentWebView())
      view->setFocus();
  });

  stackedWidget = new QStackedWidget(this);

  tabBar = new QTabBar(this);
  tabBar->setTabsClosable(true);

  layout->addWidget(tabBar);
  layout->addWidget(urlBar);
  layout->addWidget(stackedWidget);

  // The file chooser terminal covers the whole window by becoming the other
  // page of this stack; the tab logic only ever sees stackedWidget
  rootStack = new QStackedWidget(this);
  rootStack->addWidget(mainContainer);
  setCentralWidget(rootStack);

  if (optFileChooser == QLatin1String("terminal")) {
    const QString problem = TermFileChooser::helperProblem();
    if (problem.isEmpty()) {
      TermFileChooser::Options o;
      o.shell = optChooserShell;
      o.fontFamily = optChooserFont;
      o.fontSize = optChooserFontSize;
      termChooser = std::make_unique<TermFileChooser>(rootStack, std::move(o));
    } else {
      qWarning().noquote()
          << "fileChooser is \"terminal\" but the chooser scripts are unusable:"
          << problem << "- using the built-in dialog";
    }
  } else if (optFileChooser != QLatin1String("builtin")) {
    qWarning() << "Unknown fileChooser" << optFileChooser
               << "(use \"builtin\" or \"terminal\"). Using builtin";
  }

  connect(tabBar, &QTabBar::currentChanged, this, [this](int i) {
    if (i >= 0 && i < stackedWidget->count())
      stackedWidget->setCurrentIndex(i);
    if (QWebEngineView *view = currentWebView())
      urlBar->setText(view->url().toString());
    else
      urlBar->clear();
  });

  connect(tabBar, &QTabBar::tabCloseRequested, this,
          &MainWindow::slotShortcutCloseTab);

  installShortcuts();
  vimToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  installVimKeys();

  profile->setHttpUserAgent(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/140.0.0.0 Safari/537.36");

  startupClock.start();

  if (adblock) {
    adblockMgr = new AdBlockManager(adblockCfg, profile, this);
    profile->setUrlRequestInterceptor(adblockMgr->interceptor());
    const auto settle = [this] {
      adblockSettled = true;
      loadStartupUrl();
    };
    connect(adblockMgr, &AdBlockManager::settled, this, settle);
    connect(adblockMgr, &AdBlockManager::scriptsInstalled, this, [this] {
      ++envGen;
      reloadStaleTabs([this](const QUrl &u) {
        return adblockCfg.scriptletHosts.contains(u.host().toLower());
      });
    });
    QTimer::singleShot(kAdblockStartupGraceMs, this, settle);
    adblockMgr->start();
  } else {
    adblockSettled = true;
  }

  addNewTab(); // usable window right away, the startup URL loads into this tab

  if (!extMgr) {
    qWarning() << "Extensions unavailable as extensionManager returned null";
  } else {
    connect(extMgr, &QWebEngineExtensionManager::loadFinished, this,
            [this, extMgr](const QWebEngineExtensionInfo &i) {
              if (!i.isLoaded())
                qWarning() << "Failed to load extension" << i.path() << ":"
                           << i.error();
              else
                extMgr->setExtensionEnabled(i, true);
              if (pendingExts > 0 && --pendingExts == 0) {
                ++envGen;
                loadStartupUrl();
                reloadStaleTabs([](const QUrl &) { return true; });
              }
            });

    for (const QString &d : optExtensionDirs)
      pendingExts += loadExtensions(d, extMgr);

    if (pendingExts > 0)
      QTimer::singleShot(kExtensionStartupGraceMs, this, [this] {
        extsGaveUp = true; // stop waiting, but keep counting real arrivals
        loadStartupUrl();
      });
  }
  QTimer::singleShot(0, this, &MainWindow::loadStartupUrl);
}

MainWindow::~MainWindow() {
  while (stackedWidget->count() > 0) {
    QWidget *w = stackedWidget->widget(0);
    stackedWidget->removeWidget(w);
    delete w;
  }
  profile->setUrlRequestInterceptor(nullptr);
}

void MainWindow::loadUrl(const QUrl &url) {
  if (!url.isEmpty())
    startupLoaded = true; // a user navigation beats the startup URL
  if (stackedWidget->count() == 0) {
    addNewTab(url);
  } else if (QWebEngineView *view = currentWebView(); view && !url.isEmpty()) {
    view->load(url);
  }
}

void MainWindow::loadStartupUrl() {
  if (startupLoaded)
    return;
  if (pendingExts > 0 && !extsGaveUp) // capped by a timer
    return;
  const QUrl url = omnibox->resolve(optStartupUrl);
  // only hosts that get list derived scriptlets care
  if (!adblockSettled &&
      adblockCfg.scriptletHosts.contains(url.host().toLower()))
    return;
  startupLoaded = true;
  loadUrl(url);
}

void MainWindow::slotShortcutNewTab() {
  addNewTab(omnibox->resolve(optStartupUrl));
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

  const auto isHttpUrl = [](const QString &s) {
    return s.startsWith("https://") || s.startsWith("http://");
  };

  const auto mkPathValidator = [](const char *missingMsg) {
    return [missingMsg](QString p) -> std::optional<QString> {
      if (p == "~" || p.startsWith("~/"))
        p = QDir::homePath() + p.mid(1);
      p = QDir(p).absolutePath();
      if (!QDir(p).exists())
        qWarning() << missingMsg << p;
      return p;
    };
  };

  if (!haveUrlInCmdLine)
    readOpt(o, "url", optStartupUrl);
  readOpt(o, "width", optWidth);
  readOpt(o, "height", optHeight);
  readOpt(o, "proxyHost", optProxyHost);
  readOpt(o, "proxyPort", optProxyPort);
  readOpt(o, "adblock", adblock);
  readOpt(o, "adblockDownload", adblockCfg.download);
  readOpt(o, "searchUrl",
          optSearchUrl); // e.g. "https://www.google.com/search?q=%s"
  readOpt(o, "tldUpdate", optTldUpdate);
  readOpt(o, "fileChooser", optFileChooser);
  readOpt(o, "chooserShell", optChooserShell);
  readOpt(o, "chooserFont", optChooserFont);
  readOpt(o, "chooserFontSize", optChooserFontSize);

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
      if (u.isEmpty() || isHttpUrl(u))
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
      [isHttpUrl](const QString &s) -> std::optional<QString> {
        if (isHttpUrl(s))
          return s;
        qWarning() << "Ignoring adblock list that is not an http(s) URL:" << s;
        return std::nullopt;
      },
      "http(s) URLs");

  readStringArray(o, "adblockDirs", adblockCfg.localDirs,
                  mkPathValidator("adblock directory does not exist (yet):"),
                  "paths");

  readStringArray(o, "extensionDirs", optExtensionDirs,
                  mkPathValidator("Extension directories do not exist: "),
                  "paths");
}
