#include "lastfmsettingsdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>
#include <QPushButton>

LastFmSettingsDialog::LastFmSettingsDialog(LastFmAPI* api, QWidget *parent)
    : QDialog(parent)
    , m_lastFmAPI(api)
{
    setWindowTitle("Last.fm Settings");
    resize(500, 350);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // API credentials group
    QGroupBox* credentialsGroup = new QGroupBox("API Credentials", this);
    QFormLayout* formLayout = new QFormLayout(credentialsGroup);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setPlaceholderText("Enter your Last.fm API key");
    formLayout->addRow("API Key:", m_apiKeyEdit);

    m_apiSecretEdit = new QLineEdit(this);
    m_apiSecretEdit->setPlaceholderText("Enter your Last.fm API secret");
    m_apiSecretEdit->setEchoMode(QLineEdit::Password);
    formLayout->addRow("API Secret:", m_apiSecretEdit);

    mainLayout->addWidget(credentialsGroup);

    // Status group
    QGroupBox* statusGroup = new QGroupBox("Status", this);
    QVBoxLayout* statusLayout = new QVBoxLayout(statusGroup);

    m_statusLabel = new QLabel("Not authenticated", this);
    m_statusLabel->setWordWrap(true);
    statusLayout->addWidget(m_statusLabel);

    mainLayout->addWidget(statusGroup);

    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();

    m_authButton = new QPushButton("Authenticate", this);
    m_logoutButton = new QPushButton("Logout", this);
    m_testButton = new QPushButton("Test Connection", this);

    buttonLayout->addWidget(m_authButton);
    buttonLayout->addWidget(m_testButton);
    buttonLayout->addWidget(m_logoutButton);
    buttonLayout->addStretch();

    mainLayout->addLayout(buttonLayout);

    // Instructions
    QGroupBox* instructionsGroup = new QGroupBox("Instructions", this);
    QVBoxLayout* instructionsLayout = new QVBoxLayout(instructionsGroup);

    QLabel* instructionsLabel = new QLabel(
        "1. Get your API key and secret at <a href='https://www.last.fm/api/account/create'>https://www.last.fm/api/account/create</a><br>"
        "2. Enter the API key and secret above<br>"
        "3. Click 'Authenticate' and authorize the application in your browser<br>"
        "4. Click 'OK' in the confirmation dialog after authorization<br>"
        "<br>"
        "Your Last.fm scrobble counts will be displayed in the track list.",
        this
    );
    instructionsLabel->setWordWrap(true);
    instructionsLabel->setOpenExternalLinks(true);
    instructionsLayout->addWidget(instructionsLabel);

    mainLayout->addWidget(instructionsGroup);

    // Close button
    QHBoxLayout* closeLayout = new QHBoxLayout();
    closeLayout->addStretch();
    QPushButton* closeButton = new QPushButton("Close", this);
    closeLayout->addWidget(closeButton);
    mainLayout->addLayout(closeLayout);

    // Connect signals
    connect(m_authButton, &QPushButton::clicked, this, &LastFmSettingsDialog::onAuthenticateClicked);
    connect(m_logoutButton, &QPushButton::clicked, this, &LastFmSettingsDialog::onLogoutClicked);
    connect(m_testButton, &QPushButton::clicked, this, &LastFmSettingsDialog::onTestConnectionClicked);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_lastFmAPI, &LastFmAPI::tokenReceived, this, &LastFmSettingsDialog::onTokenReceived);
    connect(m_lastFmAPI, &LastFmAPI::authenticated, this, &LastFmSettingsDialog::onAuthenticated);
    connect(m_lastFmAPI, &LastFmAPI::authenticationFailed, this, &LastFmSettingsDialog::onAuthenticationFailed);

    // Load settings and update UI
    loadSettings();
    updateAuthState();
}

void LastFmSettingsDialog::loadSettings()
{
    QSettings settings;

    QString apiKey = settings.value("LastFm/apiKey").toString();
    QString apiSecret = settings.value("LastFm/apiSecret").toString();

    m_apiKeyEdit->setText(apiKey);
    m_apiSecretEdit->setText(apiSecret);

    // Apply to API
    m_lastFmAPI->setApiKey(apiKey);
    m_lastFmAPI->setApiSecret(apiSecret);
}

void LastFmSettingsDialog::saveSettings()
{
    QSettings settings;

    settings.setValue("LastFm/apiKey", m_apiKeyEdit->text());
    settings.setValue("LastFm/apiSecret", m_apiSecretEdit->text());

    // Apply to API
    m_lastFmAPI->setApiKey(m_apiKeyEdit->text());
    m_lastFmAPI->setApiSecret(m_apiSecretEdit->text());
}

void LastFmSettingsDialog::updateAuthState()
{
    bool authenticated = m_lastFmAPI->isAuthenticated();

    if (authenticated) {
        m_statusLabel->setText(QString("✓ Authenticated as: <b>%1</b>").arg(m_lastFmAPI->username()));
        m_statusLabel->setStyleSheet("QLabel { color: green; }");
        m_authButton->setEnabled(false);
        m_logoutButton->setEnabled(true);
    } else {
        m_statusLabel->setText("Not authenticated");
        m_statusLabel->setStyleSheet("QLabel { color: gray; }");
        m_authButton->setEnabled(true);
        m_logoutButton->setEnabled(false);
    }
}

void LastFmSettingsDialog::onAuthenticateClicked()
{
    // Save current settings
    saveSettings();

    if (m_apiKeyEdit->text().isEmpty() || m_apiSecretEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Missing Credentials",
                           "Please enter both API key and API secret before authenticating.");
        return;
    }

    m_statusLabel->setText("Requesting authentication token...");
    m_statusLabel->setStyleSheet("QLabel { color: blue; }");
    m_authButton->setEnabled(false);

    // Start authentication flow
    m_lastFmAPI->getToken();
}

void LastFmSettingsDialog::onTokenReceived(const QString& token)
{
    m_pendingToken = token;

    // Open browser to authorization URL
    QString authUrl = QString("http://www.last.fm/api/auth/?api_key=%1&token=%2")
                       .arg(m_lastFmAPI->apiKey())
                       .arg(token);

    QDesktopServices::openUrl(QUrl(authUrl));

    // Show message box asking user to authorize
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Authorize in Browser");
    msgBox.setText("A browser window has been opened for you to authorize this application.\n\n"
                   "Please authorize the application in your browser, then click OK.");
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Ok);

    int result = msgBox.exec();

    if (result == QMessageBox::Ok) {
        m_statusLabel->setText("Completing authentication...");
        m_lastFmAPI->getSession(token);
    } else {
        m_statusLabel->setText("Authentication cancelled");
        m_statusLabel->setStyleSheet("QLabel { color: gray; }");
        m_authButton->setEnabled(true);
    }
}

void LastFmSettingsDialog::onAuthenticated(const QString& username)
{
    // Save session key
    QSettings settings;
    settings.setValue("LastFm/sessionKey", m_lastFmAPI->sessionKey());
    settings.setValue("LastFm/username", username);

    updateAuthState();

    QMessageBox::information(this, "Authentication Successful",
                            QString("Successfully authenticated as: %1").arg(username));
}

void LastFmSettingsDialog::onAuthenticationFailed(const QString& error)
{
    m_statusLabel->setText(QString("Authentication failed: %1").arg(error));
    m_statusLabel->setStyleSheet("QLabel { color: red; }");
    m_authButton->setEnabled(true);

    QMessageBox::critical(this, "Authentication Failed",
                         QString("Failed to authenticate with Last.fm:\n%1").arg(error));
}

void LastFmSettingsDialog::onLogoutClicked()
{
    QMessageBox::StandardButton reply = QMessageBox::question(this, "Logout",
                                                              "Are you sure you want to logout from Last.fm?",
                                                              QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        m_lastFmAPI->logout();

        // Clear saved session
        QSettings settings;
        settings.remove("LastFm/sessionKey");
        settings.remove("LastFm/username");

        updateAuthState();
    }
}

void LastFmSettingsDialog::onTestConnectionClicked()
{
    saveSettings();

    if (!m_lastFmAPI->isAuthenticated()) {
        QMessageBox::warning(this, "Not Authenticated",
                           "Please authenticate first before testing the connection.");
        return;
    }

    m_statusLabel->setText("Testing connection...");
    m_statusLabel->setStyleSheet("QLabel { color: blue; }");

    // Test by fetching user info
    m_lastFmAPI->getUserInfo(m_lastFmAPI->username());

    // We'll get the result via signal, but for simplicity just show success here
    QMessageBox::information(this, "Test Connection",
                            "Connection test initiated. Check the debug log for results.");

    updateAuthState();
}
