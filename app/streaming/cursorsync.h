#pragma once

#include <QImage>
#include <QObject>
#include <QSslCertificate>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QThread;
class QTimer;
class CursorSyncWorker;

// SDL user event carrying a CursorUpdate* in data1, which the receiver deletes
#define SDL_CODE_HOST_CURSOR 106

struct CursorUpdate
{
    QImage image;
    int hotX;
    int hotY;
    bool visible;
};

// Has a Sunshine host leave its pointer out of the video and send the pointer's
// image instead, so this PC draws it without waiting for the stream. Used in
// remote desktop mouse mode; hosts without support keep drawing the pointer.
class CursorSync : public QObject
{
    Q_OBJECT

public:
    CursorSync(QUrl hostBaseUrl, QSslCertificate serverCert, int streamWidth, bool local);
    ~CursorSync();

    // Whether this PC draws the pointer; the host draws it again when this is false
    void setLocal(bool local);

private:
    QThread* m_Thread;
    CursorSyncWorker* m_Worker;
};

class CursorSyncWorker : public QObject
{
    Q_OBJECT

public:
    CursorSyncWorker(QUrl hostBaseUrl, QSslCertificate serverCert, int streamWidth, bool local);

public slots:
    void start();
    void setLocal(bool local);

private:
    void poll();
    void handleReply(QNetworkReply* reply);
    void retryLater();

    QUrl m_HostBaseUrl;
    QSslCertificate m_ServerCert;
    int m_StreamWidth;
    bool m_Local;
    bool m_Unsupported;
    QNetworkAccessManager* m_Nam;
    QNetworkReply* m_Reply;
    QTimer* m_RetryTimer;
    qint64 m_Seq;
    bool m_HaveSeq;
};
