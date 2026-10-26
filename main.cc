#include "mainwindow.hh"
#include <qapplication.h>

int main(int argc, char *argv[]) {
  QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("framebuffer-browser"));
  app.setOrganizationDomain(QStringLiteral("framebuffer-browser"));
  MainWindow window;
  window.show();
  return app.exec();
}
