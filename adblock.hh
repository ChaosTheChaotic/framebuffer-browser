#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

#include "adblock/include/fbb_adblock.h"

class QNetworkAccessManager;
class QNetworkReply;

struct AdBlockConfig {
  bool download = false; // fetch + refresh the remote lists
  int ttlHours = 96;     // refresh period for lists without "! Expires:"
  QString cacheDir;      // holds lists/ and meta.json
  QStringList lists;     // https URLs of the lists to cache and use
  QStringList localDirs; // extra directories scanned for *.txt, never touched.

  // Scriptlet rules (##+js) need the scriptlet bodies: an adblock-rust
  // resources.json, downloaded and cached like a list. Empty = no scriptlets.
  QString resourcesUrl;
  // Hosts that get scriptlet + cosmetic injection. The script for each is built
  // from the lists whenever the engine is rebuilt and installed as a user
  // script guarded by location.hostname, so it is in place before the page's
  // own JS. A YouTube host in this list also installs adblock_youtube.js (the
  // built-in YouTube handling); leave YouTube out to turn that off.
  QStringList scriptletHosts;

  // Everything that is downloaded and cached: the lists plus the resources
  // file.
  QStringList sources() const {
    QStringList s = lists;
    if (!resourcesUrl.isEmpty())
      s << resourcesUrl;
    return s;
  }
  static QString defaultResourcesUrl();
  static QStringList defaultScriptletHosts();

  // uBlock Origin's default-enabled lists
  static QStringList defaultLists();
  static QString defaultCacheDir();
};

struct ListMeta {
  QString etag;
  QString lastModified;
  qint64 fetched = 0; // unix seconds of the last download / revalidation
  qint64 ttl = 0;     // seconds the cached copy stays fresh
};
using MetaMap = QHash<QString, ListMeta>; // keyed by URL

// One finished download, waiting to be persisted by the worker.
struct Fetched {
  QString url;
  bool notModified = false; // HTTP 304: keep the file, just renew the timestamp
  QByteArray body;
  QString etag;
  QString lastModified;
  qint64 fetchedAt = 0;
};

struct HostScript {
  QString host;
  QString js;
};

struct JobResult {
  MetaMap meta;
  QList<HostScript> scripts; // only filled when built
  QByteArray localSig;       // fingerprint of the local folders this job saw
  int lists = 0;
  bool built = false;
  bool unchanged = false;
};

class AdBlockInterceptor : public QWebEngineUrlRequestInterceptor {
public:
  AdBlockInterceptor();
  ~AdBlockInterceptor() override;

  Fbbadblock *handle() const { return engine_; }
  bool ready() const { return fbb_adblock_ready(engine_); }
  void interceptRequest(QWebEngineUrlRequestInfo &info) override;

private:
  Fbbadblock *engine_;
};

class AdBlockManager : public QObject {
  Q_OBJECT
public:
  explicit AdBlockManager(AdBlockConfig cfg, QObject *parent = nullptr);
  // Waits for a build that is still running
  ~AdBlockManager() override;

  AdBlockInterceptor *interceptor() { return interceptor_; }

  // Returns immediately. Loads the cached lists in the background and, if
  // downloading is enabled, refreshes whatever has expired afterwards.
  void start();

signals:
  // Emitted once, when the first load from disk is done.
  void settled();

private:
  void startJob(QList<Fetched> fresh);
  void onJobFinished();
  void installScripts(const QList<HostScript> &scripts);
  void installYoutubeScript();
  void checkForUpdates();
  void fetch(const QString &url);
  void onReply(QNetworkReply *reply, const QString &url);
  void finishCycle();
  void scheduleNextCheck();

  AdBlockConfig cfg_;
  AdBlockInterceptor *interceptor_;
  QFutureWatcher<JobResult> watcher_;
  QTimer refreshTimer_;
  QNetworkAccessManager *nam_ = nullptr;

  MetaMap meta_;          // GUI-thread copy of the worker's last result
  QByteArray localSig_;   // local-folder fingerprint of the last finished job
  QList<Fetched> queued_; // downloaded, waiting for a free worker
  QList<Fetched> batch_;  // results of the download cycle in progress
  int inFlight_ = 0;
  int failures_ = 0;
  int retryMs_ = 0;
  bool jobRunning_ = false;
  bool firstJobDone_ = false;
  bool updatesStarted_ = false;
};
