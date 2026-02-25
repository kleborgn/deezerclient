#include "audiosettingsdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QMessageBox>

AudioSettingsDialog::AudioSettingsDialog(AudioEngine* engine, QWidget *parent)
    : QDialog(parent)
    , m_audioEngine(engine)
{
    setWindowTitle("Audio Output Settings");
    resize(480, 300);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // Output mode group
    QGroupBox* outputGroup = new QGroupBox("Output Mode", this);
    QFormLayout* formLayout = new QFormLayout(outputGroup);

    m_outputModeCombo = new QComboBox(this);
    m_outputModeCombo->addItem("DirectSound (default)", AudioEngine::OutputDirectSound);
    m_outputModeCombo->addItem("WASAPI Shared", AudioEngine::OutputWasapiShared);
    m_outputModeCombo->addItem("WASAPI Exclusive", AudioEngine::OutputWasapiExclusive);
    formLayout->addRow("Mode:", m_outputModeCombo);

    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setEnabled(false);
    formLayout->addRow("Device:", m_deviceCombo);

    mainLayout->addWidget(outputGroup);

    // Info group
    QGroupBox* infoGroup = new QGroupBox("Device Info", this);
    QVBoxLayout* infoLayout = new QVBoxLayout(infoGroup);

    m_infoLabel = new QLabel("Select an output mode", this);
    m_infoLabel->setWordWrap(true);
    infoLayout->addWidget(m_infoLabel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    infoLayout->addWidget(m_statusLabel);

    mainLayout->addWidget(infoGroup);

    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    m_applyButton = new QPushButton("Apply", this);
    QPushButton* closeButton = new QPushButton("Close", this);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_applyButton);
    buttonLayout->addWidget(closeButton);
    mainLayout->addLayout(buttonLayout);

    // Connections
    connect(m_outputModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AudioSettingsDialog::onOutputModeChanged);
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateInfo(); });
    connect(m_applyButton, &QPushButton::clicked, this, &AudioSettingsDialog::onApplyClicked);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    // Populate devices
    populateDevices();

    // Set current mode
    int currentMode = static_cast<int>(m_audioEngine->outputMode());
    for (int i = 0; i < m_outputModeCombo->count(); i++) {
        if (m_outputModeCombo->itemData(i).toInt() == currentMode) {
            m_outputModeCombo->setCurrentIndex(i);
            break;
        }
    }

    // Select current device
    if (m_audioEngine->wasapiDeviceIndex() >= 0) {
        for (int i = 0; i < m_devices.size(); i++) {
            if (m_devices[i].index == m_audioEngine->wasapiDeviceIndex()) {
                m_deviceCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    updateInfo();
}

void AudioSettingsDialog::populateDevices()
{
    m_devices = AudioEngine::enumerateWasapiDevices();
    m_deviceCombo->clear();

    for (const auto& dev : m_devices) {
        QString label = dev.name;
        if (dev.isDefault)
            label += " (Default)";
        m_deviceCombo->addItem(label, dev.index);
    }

    if (m_devices.isEmpty()) {
        m_deviceCombo->addItem("No WASAPI devices found");
    }
}

void AudioSettingsDialog::onOutputModeChanged(int index)
{
    auto mode = static_cast<AudioEngine::OutputMode>(m_outputModeCombo->itemData(index).toInt());
    bool isWasapi = (mode != AudioEngine::OutputDirectSound);
    m_deviceCombo->setEnabled(isWasapi && !m_devices.isEmpty());
    updateInfo();
}

void AudioSettingsDialog::updateInfo()
{
    auto mode = static_cast<AudioEngine::OutputMode>(
        m_outputModeCombo->currentData().toInt());

    QString info;
    if (mode == AudioEngine::OutputDirectSound) {
        info = "Standard Windows audio output.\n"
               "Compatible with all applications, moderate latency.";
    } else if (mode == AudioEngine::OutputWasapiShared) {
        info = "WASAPI Shared mode.\n"
               "Lower latency, audio is resampled to the system sample rate.\n"
               "Other applications can still use the audio device.";
    } else {
        info = "WASAPI Exclusive mode.\n"
               "Lowest latency, bit-perfect output at the device's native rate.\n"
               "Other applications will be silenced while this app has exclusive access.";
    }

    // Show device info if WASAPI selected
    if (mode != AudioEngine::OutputDirectSound && m_deviceCombo->currentIndex() >= 0
        && m_deviceCombo->currentIndex() < m_devices.size()) {
        const auto& dev = m_devices[m_deviceCombo->currentIndex()];
        info += QString("\n\nDevice: %1\nSample Rate: %2 Hz | Channels: %3")
                    .arg(dev.name)
                    .arg(dev.mixfreq)
                    .arg(dev.mixchans);
    }

    m_infoLabel->setText(info);

    // Current status
    QString statusText;
    auto currentMode = m_audioEngine->outputMode();
    if (currentMode == AudioEngine::OutputDirectSound) {
        statusText = "Currently active: DirectSound";
    } else if (currentMode == AudioEngine::OutputWasapiShared) {
        statusText = QString("Currently active: WASAPI Shared at %1 Hz").arg(m_audioEngine->outputSampleRate());
    } else {
        statusText = QString("Currently active: WASAPI Exclusive at %1 Hz").arg(m_audioEngine->outputSampleRate());
    }
    m_statusLabel->setText(statusText);
    m_statusLabel->setStyleSheet("QLabel { color: green; }");
}

void AudioSettingsDialog::onApplyClicked()
{
    auto mode = static_cast<AudioEngine::OutputMode>(
        m_outputModeCombo->currentData().toInt());

    int deviceIndex = -1;
    if (mode != AudioEngine::OutputDirectSound &&
        m_deviceCombo->currentIndex() >= 0 &&
        m_deviceCombo->currentIndex() < m_devices.size()) {
        deviceIndex = m_devices[m_deviceCombo->currentIndex()].index;
    }

    // Save to settings
    QSettings settings;
    settings.setValue("Audio/outputMode", static_cast<int>(mode));
    settings.setValue("Audio/wasapiDeviceIndex", deviceIndex);

    // Apply
    m_applyButton->setEnabled(false);
    m_applyButton->setText("Applying...");

    bool ok = m_audioEngine->reinitialize(mode, deviceIndex);

    m_applyButton->setEnabled(true);
    m_applyButton->setText("Apply");

    if (ok) {
        updateInfo();
        QMessageBox::information(this, "Audio Output",
            QString("Output mode changed to %1").arg(m_outputModeCombo->currentText()));
    } else {
        updateInfo();
        QMessageBox::warning(this, "Audio Output",
            "Failed to switch output mode. Fell back to DirectSound.");
        // Update combo to reflect actual mode
        for (int i = 0; i < m_outputModeCombo->count(); i++) {
            if (m_outputModeCombo->itemData(i).toInt() == static_cast<int>(m_audioEngine->outputMode())) {
                m_outputModeCombo->setCurrentIndex(i);
                break;
            }
        }
    }
}
