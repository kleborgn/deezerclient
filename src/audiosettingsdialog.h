#ifndef AUDIOSETTINGSDIALOG_H
#define AUDIOSETTINGSDIALOG_H

#include <QDialog>
#include "audioengine.h"

class QComboBox;
class QLabel;
class QPushButton;

class AudioSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AudioSettingsDialog(AudioEngine* engine, QWidget *parent = nullptr);

private slots:
    void onOutputModeChanged(int index);
    void onApplyClicked();

private:
    void populateDevices();
    void updateInfo();

    AudioEngine* m_audioEngine;

    QComboBox* m_outputModeCombo;
    QComboBox* m_deviceCombo;
    QLabel* m_infoLabel;
    QLabel* m_statusLabel;
    QPushButton* m_applyButton;

    QList<AudioEngine::AudioDevice> m_devices;
};

#endif // AUDIOSETTINGSDIALOG_H
