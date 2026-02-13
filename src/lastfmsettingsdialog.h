#ifndef LASTFMSETTINGSDIALOG_H
#define LASTFMSETTINGSDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include "lastfmapi.h"

class LastFmSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LastFmSettingsDialog(LastFmAPI* api, QWidget *parent = nullptr);

private slots:
    void onAuthenticateClicked();
    void onLogoutClicked();
    void onTestConnectionClicked();
    void onTokenReceived(const QString& token);
    void onAuthenticated(const QString& username);
    void onAuthenticationFailed(const QString& error);

private:
    void loadSettings();
    void saveSettings();
    void updateAuthState();

    LastFmAPI* m_lastFmAPI;

    QLineEdit* m_apiKeyEdit;
    QLineEdit* m_apiSecretEdit;
    QLabel* m_statusLabel;
    QPushButton* m_authButton;
    QPushButton* m_logoutButton;
    QPushButton* m_testButton;

    QString m_pendingToken;  // Store token while waiting for user authorization
};

#endif // LASTFMSETTINGSDIALOG_H
