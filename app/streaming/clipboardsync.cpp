#include "clipboardsync.h"
#include "backend/identitymanager.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QtDebug>

#include <string>

#ifdef Q_OS_WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#define TICK_INTERVAL_MS 250
#define TICKS_PER_POLL 2
#define MAX_ITEM_SIZE (32 * 1024 * 1024)
#define MAX_TRANSFER_SIZE (512LL * 1024 * 1024)
#define TEXT_MIME "text/plain;charset=utf-8"
#define PNG_MIME "image/png"
#define FILES_MIME "application/x-moonlight-files+json"
#define SEQUENCE_HEADER "X-Clipboard-Seq"

static QByteArray hashItem(const ClipboardItem& item)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(item.mimeType);
    hash.addData(QByteArray(1, '\0'));
    hash.addData(item.data);
    return hash.result();
}

static int httpStatus(QNetworkReply* reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// Downloads land in a Moonlight folder under Downloads, never overwriting earlier ones
static QString downloadPath(QString name)
{
    name = QFileInfo(name).fileName();
    static const QString reserved = QStringLiteral("<>:\"/\\|?*");
    for (QChar& c : name) {
        if (c.unicode() < 0x20 || reserved.contains(c)) {
            c = '_';
        }
    }
    if (name.isEmpty() || name == "." || name == "..") {
        name = "file";
    }

    QDir dir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    dir.mkpath("Moonlight");
    dir.cd("Moonlight");

    QFileInfo info(name);
    QString path = dir.filePath(name);
    for (int copy = 2; QFileInfo::exists(path); copy++) {
        QString suffix = info.suffix().isEmpty() ? QString() : "." + info.suffix();
        path = dir.filePath(QString("%1 (%2)%3").arg(info.completeBaseName()).arg(copy).arg(suffix));
    }
    return path;
}

ClipboardSync::ClipboardSync(QUrl hostBaseUrl, QSslCertificate serverCert)
    : m_Thread(new QThread()),
      m_Worker(new ClipboardSyncWorker(hostBaseUrl, serverCert))
{
    m_Thread->setObjectName("Clipboard sync");

    m_Worker->moveToThread(m_Thread);
    connect(m_Thread, &QThread::started, m_Worker, &ClipboardSyncWorker::start);
    connect(m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread->start(QThread::LowPriority);
}

ClipboardSync::~ClipboardSync()
{
    m_Thread->quit();
    m_Thread->wait();
    delete m_Thread;
}

void ClipboardSync::sendFiles(const QStringList& paths)
{
    QMetaObject::invokeMethod(m_Worker, "sendFiles", Qt::QueuedConnection, Q_ARG(QStringList, paths));
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
      m_Window(nullptr),
      m_TransferReply(nullptr),
      m_DownloadSeq(0),
      m_PendingDownloadSeq(0)
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
            if (item.mimeType == FILES_MIME) {
                m_LastHash = hashItem(item);
                QStringList paths;
                for (const QJsonValue& path : QJsonDocument::fromJson(item.data).array()) {
                    paths.append(path.toString());
                }
                sendFiles(paths);
            }
            else {
                upload(item);
            }
        }
    }
#endif

    if (++m_Ticks % TICKS_PER_POLL == 0) {
        poll();
    }
}

QNetworkRequest ClipboardSyncWorker::makeRequest(const QString& path, const QString& query)
{
    QUrl url(m_HostBaseUrl);
    url.setPath(path);
    url.setQuery(query, QUrl::StrictMode);

    QNetworkRequest request(url);
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    return request;
}

void ClipboardSyncWorker::poll()
{
    if (m_Nam == nullptr || m_PollReply != nullptr) {
        return;
    }

    QNetworkRequest request = makeRequest("/actions/clipboard", m_HaveRemoteSeq ? QString("since=%1").arg(m_RemoteSeq) : QString());
    request.setTransferTimeout(10000);
    m_PollReply = m_Nam->get(request);

    QNetworkReply* reply = m_PollReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_PollReply = nullptr;
        handlePollReply(reply);
        reply->deleteLater();
    });
}

void ClipboardSyncWorker::handlePollReply(QNetworkReply* reply)
{
    int status = httpStatus(reply);
    if (status != 200 && status != 204) {
        isFatal(reply);
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
    else if (contentType.startsWith(FILES_MIME)) {
        item.mimeType = FILES_MIME;
    }
    else {
        return;
    }
    item.data = reply->readAll();

    QByteArray hash = hashItem(item);
    if (hash == m_LastHash) {
        return;
    }

    if (item.mimeType == FILES_MIME) {
        m_LastHash = hash;
        startDownload(seq, QJsonDocument::fromJson(item.data).array());
    }
    else if (writeLocal(item)) {
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

    QNetworkRequest request = makeRequest("/actions/clipboard", QString());
    request.setHeader(QNetworkRequest::ContentTypeHeader, item.mimeType);
    request.setTransferTimeout(60000);

    if (item.mimeType != FILES_MIME) {
        m_LastHash = hashItem(item);
    }
    m_UploadReply = m_Nam->post(request, item.data);

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
    if (httpStatus(reply) != 200) {
        isFatal(reply);
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

bool ClipboardSyncWorker::isFatal(QNetworkReply* reply)
{
    int status = httpStatus(reply);

    switch (status) {
    case 403: // This device isn't allowed to share the clipboard
    case 404: // Host doesn't support clipboard sharing
    case 405:
    case 501:
        qInfo() << "Clipboard sync is unavailable on this host:" << status << reply->readAll();
        if (m_Timer != nullptr) {
            m_Timer->stop();
        }
        return true;
    default:
        // Not streaming yet (409), connection hiccups and timeouts are retried later
        return false;
    }
}

bool ClipboardSyncWorker::transferActive() const
{
    return m_TransferReply != nullptr || !m_UploadQueue.isEmpty() || !m_DownloadQueue.isEmpty();
}

void ClipboardSyncWorker::sendFiles(QStringList paths)
{
    if (m_Nam == nullptr) {
        return;
    }

    QStringList files;
    qint64 totalSize = 0;
    for (const QString& path : paths) {
        QFileInfo info(path);
        if (info.isFile()) {
            files.append(info.absoluteFilePath());
            totalSize += info.size();
        }
    }
    if (files.isEmpty()) {
        return;
    }
    if (totalSize > MAX_TRANSFER_SIZE) {
        qInfo() << "Not sending" << files.size() << "files to the host: too large" << totalSize;
        return;
    }

    if (transferActive()) {
        m_PendingUploadFiles = files;
        return;
    }

    m_UploadQueue = files;
    m_UploadedHostPaths.clear();
    uploadNextFile();
}

void ClipboardSyncWorker::uploadNextFile()
{
    if (m_UploadQueue.isEmpty()) {
        // Every file is on the host now, so put them on its clipboard
        ClipboardItem item;
        item.mimeType = FILES_MIME;
        item.data = QJsonDocument(QJsonArray::fromStringList(m_UploadedHostPaths)).toJson(QJsonDocument::Compact);
        upload(item);
        finishTransfer();
        return;
    }

    QString path = m_UploadQueue.takeFirst();
    auto file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly)) {
        qWarning() << "Unable to read" << path << "for the host";
        delete file;
        m_UploadQueue.clear();
        finishTransfer();
        return;
    }

    QUrlQuery query;
    query.addQueryItem("name", QFileInfo(path).fileName());
    QNetworkRequest request = makeRequest("/actions/files", query.toString(QUrl::FullyEncoded));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    m_TransferReply = m_Nam->post(request, file);
    file->setParent(m_TransferReply);

    QNetworkReply* reply = m_TransferReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, path]() {
        m_TransferReply = nullptr;
        reply->deleteLater();

        if (httpStatus(reply) == 200) {
            m_UploadedHostPaths.append(QString::fromUtf8(reply->readAll()));
            uploadNextFile();
        }
        else {
            qWarning() << "Sending" << path << "to the host failed:" << httpStatus(reply) << reply->errorString();
            isFatal(reply);
            m_UploadQueue.clear();
            finishTransfer();
        }
    });
}

void ClipboardSyncWorker::startDownload(qint64 seq, const QJsonArray& files)
{
    qint64 totalSize = 0;
    for (const QJsonValue& file : files) {
        totalSize += file.toObject().value("size").toVariant().toLongLong();
    }
    if (files.isEmpty() || totalSize > MAX_TRANSFER_SIZE) {
        return;
    }

    if (transferActive()) {
        m_PendingDownload = files;
        m_PendingDownloadSeq = seq;
        return;
    }

    m_DownloadSeq = seq;
    m_DownloadQueue = files;
    m_DownloadedPaths.clear();
    downloadNextFile();
}

void ClipboardSyncWorker::downloadNextFile()
{
    if (m_DownloadQueue.isEmpty()) {
        if (writeLocalFiles(m_DownloadedPaths)) {
            qInfo() << "Copied" << m_DownloadedPaths.size() << "files from the host";
        }
        finishTransfer();
        return;
    }

    QJsonObject entry = m_DownloadQueue.takeAt(0).toObject();
    QString path = downloadPath(entry.value("name").toString());
    auto file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to save" << path;
        delete file;
        m_DownloadQueue = QJsonArray();
        finishTransfer();
        return;
    }

    QNetworkRequest request = makeRequest("/actions/clipboard/file",
                                          QString("seq=%1&index=%2").arg(m_DownloadSeq).arg(entry.value("index").toInt()));
    m_TransferReply = m_Nam->get(request);
    file->setParent(m_TransferReply);

    QNetworkReply* reply = m_TransferReply;
    connect(reply, &QNetworkReply::readyRead, this, [reply, file]() {
        if (httpStatus(reply) == 200) {
            file->write(reply->readAll());
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, path]() {
        m_TransferReply = nullptr;
        reply->deleteLater();

        if (httpStatus(reply) == 200 && reply->error() == QNetworkReply::NoError) {
            file->write(reply->readAll());
            file->close();
            m_DownloadedPaths.append(path);
            downloadNextFile();
        }
        else {
            // A 409 means the host clipboard changed mid-transfer; the next poll offers the new contents
            file->remove();
            isFatal(reply);
            m_DownloadQueue = QJsonArray();
            finishTransfer();
        }
    });
}

void ClipboardSyncWorker::finishTransfer()
{
    if (transferActive()) {
        return;
    }

    if (!m_PendingUploadFiles.isEmpty()) {
        QStringList files = m_PendingUploadFiles;
        m_PendingUploadFiles.clear();
        sendFiles(files);
    }
    else if (!m_PendingDownload.isEmpty()) {
        QJsonArray files = m_PendingDownload;
        m_PendingDownload = QJsonArray();
        startDownload(m_PendingDownloadSeq, files);
    }
}

#ifdef Q_OS_WIN32

static UINT pngClipboardFormat()
{
    static UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

static UINT dropEffectClipboardFormat()
{
    static UINT format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
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

static QStringList droppedFiles(HANDLE handle)
{
    QStringList paths;
    QByteArray bytes = globalBytes(handle);
    if (bytes.size() < static_cast<qsizetype>(sizeof(DROPFILES))) {
        return paths;
    }

    auto drop = reinterpret_cast<const DROPFILES*>(bytes.constData());
    if (!drop->fWide || drop->pFiles >= static_cast<DWORD>(bytes.size())) {
        return paths;
    }

    // A double-NUL-terminated list of wide paths follows the header
    auto name = reinterpret_cast<const wchar_t*>(bytes.constData() + drop->pFiles);
    auto end = reinterpret_cast<const wchar_t*>(bytes.constData() + bytes.size());
    while (name < end && *name != L'\0') {
        const wchar_t* terminator = name;
        while (terminator < end && *terminator != L'\0') {
            terminator++;
        }
        paths.append(QString::fromWCharArray(name, static_cast<qsizetype>(terminator - name)));
        name = terminator + 1;
    }
    return paths;
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

    // Explorer also puts the file names on the clipboard as text, so files come first
    QStringList files = droppedFiles(GetClipboardData(CF_HDROP));
    if (!files.isEmpty()) {
        item.mimeType = FILES_MIME;
        item.data = QJsonDocument(QJsonArray::fromStringList(files)).toJson(QJsonDocument::Compact);
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

bool ClipboardSyncWorker::writeLocalFiles(const QStringList& paths)
{
    if (paths.isEmpty()) {
        return false;
    }

    std::wstring list;
    for (const QString& path : paths) {
        list += QDir::toNativeSeparators(path).toStdWString();
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    QByteArray drop(sizeof(DROPFILES), '\0');
    auto header = reinterpret_cast<DROPFILES*>(drop.data());
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    drop.append(reinterpret_cast<const char*>(list.data()), static_cast<qsizetype>(list.size() * sizeof(wchar_t)));

    bool written = false;
    {
        ClipboardLock lock(static_cast<HWND>(m_Window));
        if (!lock.isOpen() || !EmptyClipboard()) {
            return false;
        }

        written = setGlobalBytes(CF_HDROP, drop.constData(), static_cast<size_t>(drop.size()));

        // Pasting in Explorer should copy the downloads, not move them
        DWORD effect = DROPEFFECT_COPY;
        setGlobalBytes(dropEffectClipboardFormat(), &effect, sizeof(effect));
    }

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

bool ClipboardSyncWorker::writeLocalFiles(const QStringList&)
{
    return false;
}

#endif
