#pragma once

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <optional>

class QNetworkAccessManager;

// TLDs, lowercase and punycode for IDNs
class TldSet {
public:
  // Replaces the set from an IANA tlds-alpha-by-domain.txt body. Returns false
	// Returns false and uses old set if text does not look like that file
  bool load(const QByteArray &text);
  bool loaded() const { return loaded_; }
  bool contains(const QString &tld) const;

private:
  QSet<QString> tlds_;
  bool loaded_ = false;
};

// Decides if input is a URL or search
// Keeps TLD list on disk and refreshed
class Omnibox : public QObject {
  Q_OBJECT
public:
  // searchTemplate must contain "%s"; empty selects defaultSearchUrl()
  explicit Omnibox(QString searchTemplate = {}, bool autoUpdate = true,
                   QObject *parent = nullptr);

  static QString defaultSearchUrl();

  // Loads the cached TLD list, then refreshes it in the background when stale
  void start();

  // Empty input gives an empty QUrl
  QUrl resolve(const QString &input) const;
  QUrl searchUrl(const QString &query) const;

  TldSet &tlds() { return tlds_; }

private:
  std::optional<QUrl> hostUrl(const QString &input) const;
  void refreshIfStale();
  void download();
  static QString cachePath();

  QString searchTemplate_;
  bool autoUpdate_;
  bool downloading_ = false;
  TldSet tlds_;
  QTimer refreshTimer_;
  QNetworkAccessManager *nam_ = nullptr;
};
