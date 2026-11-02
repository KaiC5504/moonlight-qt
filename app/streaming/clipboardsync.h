#pragma once

#include <QByteArray>
#include <QObject>
#include <QSslCertificate>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QThread;
class QTimer;

// Keeps the clipboard of this PC and a Sunshine host in step while a stream runs.
// The host decides per device whether this is allowed; hosts without support are left alone.
class ClipboardSync : public QObject
{
    Q_OBJECT

public:
    ClipboardSync(QUrl hostBaseUrl, QSslCertificate serverCert);
    ~ClipboardSync();

private:
    QThread* m_Thread;
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

private:
    void stop();

private slots:
    void tick();

private:
    void poll();
    void handlePollReply(QNetworkReply* reply);
    void upload(const ClipboardItem& item);
    void handleUploadReply(QNetworkReply* reply);
    void disable(QNetworkReply* reply);
    QNetworkReply* sendRequest(const QString& query, const ClipboardItem* body);

    ClipboardItem readLocal();
    bool writeLocal(const ClipboardItem& item);

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
};
