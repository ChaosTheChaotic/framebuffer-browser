#pragma once

#include <QString>
#include <QStringList>

class QStackedWidget;

class TermFileChooser {
public:
  struct Options {
    QString shell;      // program to run, must accept -i, empty = $SHELL
    QString fontFamily; // empty = the systems fixed font
    int fontSize = 13;  // points, <= 0 keeps the font's own size
  };

  struct Request {
    bool multiple = false;
    bool directory = false; // a directory is wanted instead of a file
    bool save = false;      // the path may not exist yet
    QString suggested;      // save, the file name the page proposes
    QStringList accept;     // MIME types/extensions the page accepts
  };

  // host is the stack holding the windows normal content
	// the terminal is added to it for the duration of a request
  TermFileChooser(QStackedWidget *host, Options opts);

  // Folder with session and yield, FBBROWSER_CHOOSER_DIR, else "chooser"
  // next to the browser binary (where the Nix package installs them)
  static QString helperDir();
  // Empty if session and yield are usable, else what is wrong with them
  static QString helperProblem();

  // Blocks in a nested event loop until the shell ends
	// Returns the chosen absolute paths, or an empty list if cancelled
  QStringList choose(const Request &req);

private:
  QStackedWidget *host_;
  Options opts_;
  QString lastDir_; // where the previous pick was, so the next starts there
  bool busy_ = false;
};
