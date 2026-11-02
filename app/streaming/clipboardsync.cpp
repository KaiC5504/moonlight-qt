#include "clipboardsync.h"
#include "backend/identitymanager.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QtDebug>

#include <string>

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

#define TICK_INTERVAL_MS 250
#define TICKS_PER_POLL 2
#define MAX_ITEM_SIZE (32 * 1024 * 1024)
#define TEXT_MIME "text/plain;charset=utf-8"
#define PNG_MIME "image/png"
#define SEQUENCE_HEADER "X-Clipboard-Seq"

static QByteArray hashItem(const ClipboardItem& item)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(item.mimeType);
    hash.addData(QByteArray(1, '\0'));
    hash.addData(item.data);
    return hash.result();
}

ClipboardSync::ClipboardSync(QUrl hostBaseUrl, QSslCertificate serverCert)
    : m_Thread(new QThread())
{
    m_Thread->setObjectName("Clipboard sync");

    auto worker = new ClipboardSyncWorker(hostBaseUrl, serverCert);
    worker->moveToThread(m_Thread);
    connect(m_Thread, &QThread::started, worker, &ClipboardSyncWorker::start);
    connect(m_Thread, &QThread::finished, worker, &QObject::deleteLater);
    m_Thread->start(QThread::LowPriority);
}

ClipboardSync::~ClipboardSync()
{
    m_Thread->quit();
    m_Thread->wait();
    delete m_Thread;
}

ClipboardSyncWorker::ClipboardSyncWorker(QUrl hostBaseUrl, QSslCertificate serverCert)
    : m_HostBaseUrl(hostBaseUrl),
      m_ServerCert(serverCert),
      m_Nam(nullptr),
      m_Timer(nullptr),
      m_PollReply(nullptr),
      m_UploadReply(nullptr),
      m_RemoteSeq(0),
      m_HaveRemoteSeq(false),
      m_LocalSeq(0),
      m_Ticks(0),
      m_Window(nullptr)
{
}

ClipboardSyncWorker::~ClipboardSyncWorker()
{
    stop();
}

void ClipboardSyncWorker::start()
{
#ifdef Q_OS_WIN32
    // SetClipboardData() fails unless the clipboard was opened by a window, so own a hidden one
    m_Window = CreateWindowExW(0, L"STATIC", L"Moonlight clipboard sync", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (m_Window == nullptr) {
        qWarning() << "Clipboard sync disabled: unable to create clipboard window" << GetLastError();
        return;
    }

    // Only changes made during the stream are shared
    m_LocalSeq = GetClipboardSequenceNumber();

    m_Nam = new QNetworkAccessManager(this);
    connect(m_Nam, &QNetworkAccessManager::sslErrors, this, [this](QNetworkReply* reply, const QList<QSslError>& errors) {
        for (const QSslError& error : errors) {
            if (error.certificate() != m_ServerCert) {
                return;
            }
        }
        reply->ignoreSslErrors(errors);
    });

    m_Timer = new QTimer(this);
    connect(m_Timer, &QTimer::timeout, this, &ClipboardSyncWorker::tick);
    m_Timer->start(TICK_INTERVAL_MS);
    poll();
#endif
}

void ClipboardSyncWorker::stop()
{
    if (m_Timer != nullptr) {
        m_Timer->stop();
    }

#ifdef Q_OS_WIN32
    if (m_Window != nullptr) {
        DestroyWindow(static_cast<HWND>(m_Window));
        m_Window = nullptr;
    }
#endif
}

void ClipboardSyncWorker::tick()
{
#ifdef Q_OS_WIN32
    quint32 localSeq = GetClipboardSequenceNumber();
    if (localSeq != m_LocalSeq) {
        m_LocalSeq = localSeq;

        ClipboardItem item = readLocal();
        if (!item.isNull() && item.data.size() <= MAX_ITEM_SIZE && hashItem(item) != m_LastHash) {
            upload(item);
        }
    }
#endif

    if (++m_Ticks % TICKS_PER_POLL == 0) {
        poll();
    }
}

QNetworkReply* ClipboardSyncWorker::sendRequest(const QString& query, const ClipboardItem* body)
{
    QUrl url(m_HostBaseUrl);
    url.setPath("/actions/clipboard");
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(10000);

    if (body != nullptr) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, body->mimeType);
        return m_Nam->post(request, body->data);
    }
    return m_Nam->get(request);
}

void ClipboardSyncWorker::poll()
{
    if (m_Nam == nullptr || m_PollReply != nullptr) {
        return;
    }

    m_PollReply = sendRequest(m_HaveRemoteSeq ? QString("since=%1").arg(m_RemoteSeq) : QString(), nullptr);
    QNetworkReply* reply = m_PollReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_PollReply = nullptr;
        handlePollReply(reply);
        reply->deleteLater();
    });
}

void ClipboardSyncWorker::handlePollReply(QNetworkReply* reply)
{
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200 && status != 204) {
        disable(reply);
        return;
    }

    bool firstPoll = !m_HaveRemoteSeq;
    bool ok;
    qint64 seq = reply->rawHeader(SEQUENCE_HEADER).toLongLong(&ok);
    if (ok) {
        m_RemoteSeq = seq;
        m_HaveRemoteSeq = true;
    }

    // The first answer is just the host's starting point. Only later changes are copied here.
    if (status != 200 || firstPoll) {
        return;
    }

    QByteArray contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().toLower();
    ClipboardItem item;
    if (contentType.startsWith("text/plain")) {
        item.mimeType = TEXT_MIME;
    }
    else if (contentType.startsWith(PNG_MIME)) {
        item.mimeType = PNG_MIME;
    }
    else {
        return;
    }
    item.data = reply->readAll();

    QByteArray hash = hashItem(item);
    if (hash == m_LastHash) {
        return;
    }

    if (writeLocal(item)) {
        m_LastHash = hash;
    }
}

void ClipboardSyncWorker::upload(const ClipboardItem& item)
{
    if (m_Nam == nullptr) {
        return;
    }

    // Only the newest local item matters if the previous upload is still running
    if (m_UploadReply != nullptr) {
        m_PendingUpload = item;
        return;
    }

    m_LastHash = hashItem(item);
    m_UploadReply = sendRequest(QString(), &item);
    QNetworkReply* reply = m_UploadReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_UploadReply = nullptr;
        handleUploadReply(reply);
        reply->deleteLater();

        if (!m_PendingUpload.isNull()) {
            ClipboardItem next = m_PendingUpload;
            m_PendingUpload = ClipboardItem();
            upload(next);
        }
    });
}

void ClipboardSyncWorker::handleUploadReply(QNetworkReply* reply)
{
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        disable(reply);
        return;
    }

    // Skip our own item when the next poll reports the host clipboard changed
    bool ok;
    qint64 seq = reply->rawHeader(SEQUENCE_HEADER).toLongLong(&ok);
    if (ok) {
        m_RemoteSeq = seq;
        m_HaveRemoteSeq = true;
    }
}

void ClipboardSyncWorker::disable(QNetworkReply* reply)
{
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    switch (status) {
    case 403: // This device isn't allowed to share the clipboard
    case 404: // Host doesn't support clipboard sharing
    case 405:
    case 501:
        qInfo() << "Clipboard sync is unavailable on this host:" << status << reply->readAll();
        if (m_Timer != nullptr) {
            m_Timer->stop();
        }
        break;
    default:
        // Not streaming yet (409), connection hiccups and timeouts are retried on the next poll
        break;
    }
}

#ifdef Q_OS_WIN32

static UINT pngClipboardFormat()
{
    static UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

class ClipboardLock
{
public:
    explicit ClipboardLock(HWND owner)
        : m_Open(false)
    {
        // Another application may be holding the clipboard briefly
        for (int i = 0; i < 10 && !m_Open; i++) {
            m_Open = OpenClipboard(owner);
            if (!m_Open) {
                Sleep(10);
            }
        }
    }

    ~ClipboardLock()
    {
        if (m_Open) {
            CloseClipboard();
        }
    }

    bool isOpen() const { return m_Open; }

private:
    bool m_Open;
};

static QByteArray globalBytes(HANDLE handle)
{
    QByteArray bytes;
    if (handle == nullptr) {
        return bytes;
    }

    SIZE_T size = GlobalSize(handle);
    const void* data = GlobalLock(handle);
    if (data != nullptr) {
        if (size <= MAX_ITEM_SIZE) {
            bytes = QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size));
        }
        GlobalUnlock(handle);
    }
    return bytes;
}

static bool setGlobalBytes(UINT format, const void* data, size_t size)
{
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size);
    if (handle == nullptr) {
        return false;
    }

    void* target = GlobalLock(handle);
    if (target == nullptr) {
        GlobalFree(handle);
        return false;
    }
    memcpy(target, data, size);
    GlobalUnlock(handle);

    if (SetClipboardData(format, handle) == nullptr) {
        GlobalFree(handle);
        return false;
    }
    return true;
}

static QByteArray dibToPng(const QByteArray& dib)
{
    if (dib.size() < static_cast<qsizetype>(sizeof(BITMAPINFOHEADER))) {
        return QByteArray();
    }

    // Rebuild a BMP file around the DIB so QImage can decode it
    auto header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.constData());
    DWORD colors = header->biClrUsed;
    if (colors == 0 && header->biBitCount <= 8) {
        colors = 1u << header->biBitCount;
    }
    DWORD masks = (header->biCompression == BI_BITFIELDS && header->biSize == sizeof(BITMAPINFOHEADER)) ? 3 * sizeof(DWORD) : 0;

    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfSize = static_cast<DWORD>(sizeof(fileHeader) + dib.size());
    fileHeader.bfOffBits = static_cast<DWORD>(sizeof(fileHeader) + header->biSize + colors * sizeof(RGBQUAD) + masks);

    QByteArray file(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file += dib;

    QImage image;
    if (!image.loadFromData(file, "BMP")) {
        return QByteArray();
    }

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

ClipboardItem ClipboardSyncWorker::readLocal()
{
    ClipboardItem item;
    ClipboardLock lock(static_cast<HWND>(m_Window));
    if (!lock.isOpen()) {
        return item;
    }

    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (auto text = static_cast<const wchar_t*>(GlobalLock(handle))) {
            QString string = QString::fromWCharArray(text);
            GlobalUnlock(handle);

            // The host is a Mac, which uses bare line feeds
            string.replace("\r\n", "\n");
            if (!string.isEmpty()) {
                item.mimeType = TEXT_MIME;
                item.data = string.toUtf8();
                return item;
            }
        }
    }

    QByteArray png = globalBytes(GetClipboardData(pngClipboardFormat()));
    if (png.isEmpty()) {
        png = dibToPng(globalBytes(GetClipboardData(CF_DIB)));
    }
    if (!png.isEmpty()) {
        item.mimeType = PNG_MIME;
        item.data = png;
    }
    return item;
}

bool ClipboardSyncWorker::writeLocal(const ClipboardItem& item)
{
    QByteArray dib;
    std::wstring text;
    if (item.mimeType == TEXT_MIME) {
        QString string = QString::fromUtf8(item.data);
        string.replace("\r\n", "\n").replace("\n", "\r\n");
        text = string.toStdWString();
    }
    else {
        // Many Windows apps only paste CF_DIB, so offer it alongside the PNG
        QImage image;
        if (!image.loadFromData(item.data, "PNG")) {
            return false;
        }
        QBuffer buffer(&dib);
        buffer.open(QIODevice::WriteOnly);
        if (!image.convertToFormat(QImage::Format_RGB32).save(&buffer, "BMP")) {
            return false;
        }
        dib.remove(0, sizeof(BITMAPFILEHEADER));
    }

    bool written = false;
    {
        ClipboardLock lock(static_cast<HWND>(m_Window));
        if (!lock.isOpen() || !EmptyClipboard()) {
            return false;
        }

        if (item.mimeType == TEXT_MIME) {
            written = setGlobalBytes(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        }
        else {
            written = setGlobalBytes(CF_DIB, dib.constData(), static_cast<size_t>(dib.size()));
            written = setGlobalBytes(pngClipboardFormat(), item.data.constData(), static_cast<size_t>(item.data.size())) || written;
        }
    }

    // Our own write must not be sent back to the host
    m_LocalSeq = GetClipboardSequenceNumber();
    return written;
}

#else

ClipboardItem ClipboardSyncWorker::readLocal()
{
    return ClipboardItem();
}

bool ClipboardSyncWorker::writeLocal(const ClipboardItem&)
{
    return false;
}

#endif
