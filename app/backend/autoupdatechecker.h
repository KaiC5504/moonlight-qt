#pragma once

#include <QObject>
#include <QNetworkAccessManager>

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();

    // Downloads the installer named in the update manifest, checks its hash, runs it and quits
    Q_INVOKABLE bool installUpdate();

    Q_PROPERTY(bool canInstallUpdate READ canInstallUpdate NOTIFY canInstallUpdateChanged)
    bool canInstallUpdate() const;

signals:
    void onUpdateAvailable(QString newVersion, QString url);
    void updateInstallFailed(QString url);
    void canInstallUpdateChanged();

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);
    void handleInstallerDownloadFinished(QNetworkReply* reply);

private:
    void parseStringToVersionQuad(QString& string, QVector<int>& version);

    int compareVersion(QVector<int>& version1, QVector<int>& version2);

    QString getPlatform();

    QVector<int> m_CurrentVersionQuad;
    QNetworkAccessManager* m_Nam;
    QString m_NewVersion;
    QString m_BrowserUrl;
    QString m_InstallerUrl;
    QByteArray m_InstallerSha256;
};
