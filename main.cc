#include "mainwindow.hh"
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <cstdio>
#include <cstring>
#include <qapplication.h>

int main(int argc, char *argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--chromium-version") == 0) {
    std::puts(qWebEngineChromiumVersion());
    return 0;
  }
  QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("framebuffer-browser"));
  app.setOrganizationDomain(QStringLiteral("framebuffer-browser"));
  MainWindow window;
  window.show();
  return app.exec();
}
