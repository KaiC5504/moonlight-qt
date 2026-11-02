#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QSslCertificate>
#include <QStringList>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QThread;
class QTimer;
class ClipboardSyncWorker;

// Keeps the clipboard of this PC and a Sunshine host in step while a stream runs,
// including copied files. The host decides per device whether this is allowed;
// hosts without support are left alone.
class ClipboardSync : public QObject
{
    Q_OBJECT

public:
    ClipboardSync(QUrl hostBaseUrl, QSslCertificate serverCert);
    ~ClipboardSync();

    // Copies files to the host and puts them on its clipboard, like dropping them onto the stream
    void sendFiles(const QStringList& paths);

private:
    QThread* m_Thread;
    ClipboardSyncWorker* m_Worker;
};

struct ClipboardItem
{
    QByteArray mimeType;
    QByteArray data;

    bool isNull() const { return mimeType.isEmpty(); }
};

class ClipboardSyncWorker : public QObject
{
    Q_OBJECT

public:
    ClipboardSyncWorker(QUrl hostBaseUrl, QSslCertificate serverCert);
    ~ClipboardSyncWorker();

public slots:
    void start();
    void sendFiles(QStringList paths);

private slots:
    void tick();

private:
    void stop();
    void poll();
    void handlePollReply(QNetworkReply* reply);
    void upload(const ClipboardItem& item);
    void handleUploadReply(QNetworkReply* reply);
    bool isFatal(QNetworkReply* reply);
    QNetworkRequest makeRequest(const QString& path, const QString& query);

    bool transferActive() const;
    void startNextTransfer();
    void uploadNextFile();
    void startDownload(qint64 seq, const QJsonArray& files);
    void downloadNextFile();
    void finishTransfer();

    ClipboardItem readLocal();
    bool writeLocal(const ClipboardItem& item);
    bool writeLocalFiles(const QStringList& paths);

    QUrl m_HostBaseUrl;
    QSslCertificate m_ServerCert;
    QNetworkAccessManager* m_Nam;
    QTimer* m_Timer;
    QNetworkReply* m_PollReply;
    QNetworkReply* m_UploadReply;
    ClipboardItem m_PendingUpload;
    qint64 m_RemoteSeq;
    bool m_HaveRemoteSeq;
    quint32 m_LocalSeq;
    QByteArray m_LastHash;
    int m_Ticks;
    void* m_Window;

    // One file transfer runs at a time; the newest request in each direction waits behind it
    QNetworkReply* m_TransferReply;
    QStringList m_UploadQueue;
    QStringList m_UploadedHostPaths;
    QStringList m_PendingUploadFiles;
    QJsonArray m_DownloadQueue;
    QStringList m_DownloadedPaths;
    qint64 m_DownloadSeq;
    QJsonArray m_PendingDownload;
    qint64 m_PendingDownloadSeq;
};
