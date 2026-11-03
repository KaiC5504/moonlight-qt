#include "cursorsync.h"
#include "backend/identitymanager.h"

#include "SDL_compat.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QtDebug>

#define SEQUENCE_HEADER "X-Cursor-Seq"
#define VISIBLE_HEADER "X-Cursor-Visible"
#define HOTSPOT_HEADER "X-Cursor-Hotspot"

// The host answers a poll within 10 seconds even when the pointer stays the same
#define POLL_TIMEOUT_MS 15000
#define RETRY_INTERVAL_MS 1000

CursorSync::CursorSync(QUrl hostBaseUrl, QSslCertificate serverCert, int streamWidth, bool local)
    : m_Thread(new QThread()),
      m_Worker(new CursorSyncWorker(hostBaseUrl, serverCert, streamWidth, local))
{
    m_Thread->setObjectName("Cursor sync");

    m_Worker->moveToThread(m_Thread);
    connect(m_Thread, &QThread::started, m_Worker, &CursorSyncWorker::start);
    connect(m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread->start();
}

CursorSync::~CursorSync()
{
    m_Thread->quit();
    m_Thread->wait();
    delete m_Thread;
}

void CursorSync::setLocal(bool local)
{
    QMetaObject::invokeMethod(m_Worker, "setLocal", Qt::QueuedConnection, Q_ARG(bool, local));
}

CursorSyncWorker::CursorSyncWorker(QUrl hostBaseUrl, QSslCertificate serverCert, int streamWidth, bool local)
    : m_HostBaseUrl(hostBaseUrl),
      m_ServerCert(serverCert),
      m_StreamWidth(streamWidth),
      m_Local(local),
      m_Unsupported(false),
      m_Nam(nullptr),
      m_Reply(nullptr),
      m_RetryTimer(nullptr),
      m_Seq(0),
      m_HaveSeq(false)
{
}

void CursorSyncWorker::start()
{
    m_Nam = new QNetworkAccessManager(this);
    connect(m_Nam, &QNetworkAccessManager::sslErrors, this, [this](QNetworkReply* reply, const QList<QSslError>& errors) {
        for (const QSslError& error : errors) {
            if (error.certificate() != m_ServerCert) {
                return;
            }
        }
        reply->ignoreSslErrors(errors);
    });

    m_RetryTimer = new QTimer(this);
    m_RetryTimer->setSingleShot(true);
    connect(m_RetryTimer, &QTimer::timeout, this, &CursorSyncWorker::poll);

    if (m_Local) {
        poll();
    }
}

void CursorSyncWorker::setLocal(bool local)
{
    if (local == m_Local) {
        return;
    }
    m_Local = local;

    // A waiting poll still carries the old mode, so replace it
    if (m_Reply != nullptr) {
        m_Reply->abort();
    }
    else {
        poll();
    }
}

void CursorSyncWorker::poll()
{
    if (m_Nam == nullptr || m_Reply != nullptr || m_Unsupported) {
        return;
    }
    m_RetryTimer->stop();

    QUrlQuery query;
    if (m_HaveSeq) {
        query.addQueryItem("since", QString::number(m_Seq));
    }
    query.addQueryItem("width", QString::number(m_StreamWidth));
    query.addQueryItem("local", m_Local ? "1" : "0");

    QUrl url(m_HostBaseUrl);
    url.setPath("/actions/cursor");
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(POLL_TIMEOUT_MS);
    m_Reply = m_Nam->get(request);

    QNetworkReply* reply = m_Reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_Reply = nullptr;
        handleReply(reply);
        reply->deleteLater();
    });
}

void CursorSyncWorker::handleReply(QNetworkReply* reply)
{
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        // Aborted by setLocal()
        poll();
        return;
    }

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403 || status == 404 || status == 501) {
        qInfo() << "Host doesn't send its pointer; it stays in the video:" << status;
        m_Unsupported = true;
        return;
    }
    if (status != 200 && status != 204) {
        // Not streaming yet, or a network error
        retryLater();
        return;
    }

    bool ok;
    qint64 seq = reply->rawHeader(SEQUENCE_HEADER).toLongLong(&ok);
    if (ok) {
        m_Seq = seq;
        m_HaveSeq = true;
    }

    // The host answers a non-local poll at once, and from then on it draws the pointer
    if (!m_Local) {
        return;
    }

    if (status == 200) {
        auto update = new CursorUpdate();
        update->visible = reply->rawHeader(VISIBLE_HEADER) != "0";
        update->hotX = update->hotY = 0;

        QList<QByteArray> hotspot = reply->rawHeader(HOTSPOT_HEADER).split(',');
        if (hotspot.size() == 2) {
            update->hotX = hotspot[0].toInt();
            update->hotY = hotspot[1].toInt();
        }

        QByteArray png = reply->readAll();
        if (!png.isEmpty()) {
            update->image = QImage::fromData(png, "PNG");
        }

        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_HOST_CURSOR;
        event.user.data1 = update;
        if (SDL_PushEvent(&event) <= 0) {
            delete update;
        }
    }

    poll();
}

void CursorSyncWorker::retryLater()
{
    m_RetryTimer->start(RETRY_INTERVAL_MS);
}
