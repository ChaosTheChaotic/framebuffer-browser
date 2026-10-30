#include "mainwindow.hh"
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <cstdio>
#include <cstring>
#include <qapplication.h>
#include <qpa/qplatformnativeinterface.h>
#include <cstring>

// Shut down evdev before qt to not segfault
static void shutdownEvdevTouch() {
  QObject *integration = QGuiApplication::platformNativeInterface();
  if (!integration) {
    qWarning("shutdownEvdevTouch: no platform integration object");
    return;
  }

  // Deleting a child mutates the parent's own list.
  const QObjectList kids = integration->children();
  bool found = false;
  for (QObject *o : kids) {
    // typeid() on a polymorphic object gives its real C++ type, so this works
    // even though the class has no Q_OBJECT (className() would say "QObject")
    if (std::strstr(typeid(*o).name(), "QEvdevTouchManager")) {
      delete o; // joins the touch thread while qApp is still alive
      found = true;
    }
  }
  if (!found) {
    qWarning("shutdownEvdevTouch: no QEvdevTouchManager among %lld children:",
             (long long)kids.size());
    for (QObject *o : kids)
      qWarning("  %s", typeid(*o).name());
  }

  // The handler queued deleteLater() for its device on this thread.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

int main(int argc, char *argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--chromium-version") == 0) {
    std::puts(qWebEngineChromiumVersion());
    return 0;
  }
  QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("framebuffer-browser"));
  app.setOrganizationDomain(QStringLiteral("framebuffer-browser"));

  int rc;
  {
    MainWindow window;
    window.show();
    rc = app.exec();
  }

  shutdownEvdevTouch();
  return rc;
}
