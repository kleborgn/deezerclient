#ifndef PLAYERCONTROLS_H
#define PLAYERCONTROLS_H

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include "audioengine.h"
#include "waveformwidget.h"

class PlayerControls : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerControls(QWidget *parent = nullptr);

    void setAudioEngine(AudioEngine* engine);

signals:
    void playClicked();
    void pauseClicked();
    void stopClicked();
    void nextClicked();
    void previousClicked();
    void seekRequested(double position);
    void volumeChanged(float volume);

private slots:
    void onPlayPauseClicked();
    void onStateChanged(AudioEngine::PlaybackState state);
    void onTrackChanged(std::shared_ptr<Track> track);
    void onPositionChanged(int seconds);
    void onPositionTick(double position);
    void onWaveformReady(const QVector<float>& peaks);
    void onVolumeSliderChanged(int value);
    void onRepeatClicked();
    void onRepeatModeChanged(AudioEngine::RepeatMode mode);

private:
    void updatePlayPauseButton();
    QString formatTime(int seconds) const;
    bool eventFilter(QObject* watched, QEvent* event) override;

    AudioEngine* m_audioEngine;

    QPushButton* m_previousButton;
    QPushButton* m_playPauseButton;
    QPushButton* m_stopButton;
    QPushButton* m_nextButton;
    QPushButton* m_repeatButton;

    WaveformWidget* m_waveformWidget;
    QSlider* m_volumeSlider;

    QLabel* m_trackInfoLabel;
    QLabel* m_positionLabel;
    QLabel* m_durationLabel;
    QLabel* m_volumeLabel;
};

#endif // PLAYERCONTROLS_H
