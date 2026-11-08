#include "filechooser.hh"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QKeySequence>
#include <QPointer>
#include <QShortcut>
#include <QStackedWidget>
#include <QTemporaryDir>
// qtermwidget installs its headers under include/qtermwidget6/
// CMakes target normally adds that directory, fall back to the prefixed path if it didnt
#if __has_include(<qtermwidget.h>)
#include <qtermwidget.h>
#else
#include <qtermwidget6/qtermwidget.h>
#endif
#include <utility>

namespace {

QString helperPath(const char *name) {
  return TermFileChooser::helperDir() + QLatin1Char('/') + QLatin1String(name);
}

QFont terminalFont(const TermFileChooser::Options &o) {
  QFont f = o.fontFamily.isEmpty()
                ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                : QFont(o.fontFamily);
  f.setStyleHint(QFont::Monospace);
  if (o.fontSize > 0)
    f.setPointSize(o.fontSize);
  return f;
}

} // namespace

TermFileChooser::TermFileChooser(QStackedWidget *host, Options opts)
    : host_(host), opts_(std::move(opts)) {}

QString TermFileChooser::helperDir() {
  const QString env = qEnvironmentVariable("FBBROWSER_CHOOSER_DIR");
  if (!env.isEmpty())
    return env;
  return QCoreApplication::applicationDirPath() + QStringLiteral("/chooser");
}

QString TermFileChooser::helperProblem() {
  const char *const names[] = {"session", "yield"};
  for (const char *name : names) {
    const QString path = helperPath(name);
    const QFileInfo fi(path); // follows symlinks, so a linked script is judged by its target
    if (!fi.exists())
      return path + QStringLiteral(" does not exist");
    if (!fi.isFile())
      return path + QStringLiteral(" is not a file");
    if (!fi.isExecutable())
      return path + QStringLiteral(" is not executable (chmod +x it)");
  }
  return {};
}

QStringList TermFileChooser::choose(const Request &req) {
  if (busy_)
    return {}; // one at a time, the terminal already owns the window

  // The result travels through a file, yield writes it, we read it once the
  // shell has ended
  QTemporaryDir tmp;
  if (!tmp.isValid()) {
    qWarning() << "File chooser: cannot create a temporary directory";
    return {};
  }
  const QString outPath = tmp.filePath(QStringLiteral("selection"));

  QString startDir = lastDir_;
  if (startDir.isEmpty() || !QFileInfo(startDir).isDir())
    startDir = QDir::homePath();

  // Added on top of the browser's own environment
	// session strips the parts of that which only make sense for the browser (preload shims, Qt platform).
  QStringList env;
  env << QStringLiteral("TERM=xterm-256color")
      << QStringLiteral("COLORTERM=truecolor")
      << QStringLiteral("YIELD_SAVE_PATH=") + outPath
      << QStringLiteral("YIELD_MODE_MULTIPLE=") +
             QString::number(req.multiple ? 1 : 0)
      << QStringLiteral("YIELD_MODE_DIR=") +
             QString::number(req.directory ? 1 : 0)
      << QStringLiteral("YIELD_MODE_SAVE=") + QString::number(req.save ? 1 : 0)
      << QStringLiteral("YIELD_SUGGESTED=") + req.suggested
      << QStringLiteral("YIELD_ACCEPT=") + req.accept.join(QStringLiteral(", "));
  if (!opts_.shell.isEmpty())
    env << QStringLiteral("FBB_CHOOSER_SHELL=") + opts_.shell;

  auto *term = new QTermWidget(0, host_); // start the shell later
  term->setTerminalFont(terminalFont(opts_));
  term->setColorScheme(QStringLiteral("Linux"));
  term->setScrollBarPosition(QTermWidget::NoScrollBar);
  term->setShellProgram(helperPath("session"));
  term->setWorkingDirectory(startDir);
  term->setEnvironment(env);

  // Same copy/paste keys as the console terminal
  const auto bind = [term](const char *seq, void (QTermWidget::*slot)()) {
    auto *sc = new QShortcut(QKeySequence(QString::fromLatin1(seq)), term);
    sc->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(sc, &QShortcut::activated, term, slot);
  };
  bind("Ctrl+Shift+C", &QTermWidget::copyClipboard);
  bind("Ctrl+Shift+V", &QTermWidget::pasteClipboard);

  QPointer<QWidget> prevPage = host_->currentWidget();
  QPointer<QWidget> prevFocus = QApplication::focusWidget();
  host_->addWidget(term);
  host_->setCurrentWidget(term);
  term->setFocus();

  busy_ = true;
  bool done = false;
  QEventLoop loop;
  QObject::connect(term, &QTermWidget::finished, &loop, [&] {
    done = true;
    loop.quit();
  });
  term->startShellProgram();
  if (!done) // the shell may already have failed to start
    loop.exec();

  // done is false if the loop was left because the application is quitting
  QStringList picked;
  if (done) {
    QFile f(outPath);
    if (f.open(QIODevice::ReadOnly)) {
      for (const QByteArray &p : f.readAll().split('\0'))
        if (!p.isEmpty())
          picked << QFile::decodeName(p);
    }
  }

  host_->removeWidget(term);
  if (prevPage)
    host_->setCurrentWidget(prevPage);
  delete term; // closing the pty also hangs up anything still running in it
  if (prevFocus)
    prevFocus->setFocus();
  busy_ = false;

  if (!req.multiple && picked.size() > 1)
    picked = picked.mid(0, 1);

  if (!picked.isEmpty()) {
    const QFileInfo fi(picked.first());
    lastDir_ = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    // Qts own dialog hands Chromium folders with a trailing slash too
    if (req.directory)
      for (QString &p : picked)
        if (!p.endsWith(QLatin1Char('/')))
          p += QLatin1Char('/');
  }
  return picked;
}
