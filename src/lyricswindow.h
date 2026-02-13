#ifndef LYRICSWINDOW_H
#define LYRICSWINDOW_H

#include <QWidget>
#include <QCloseEvent>
#include <QJsonArray>
#include <memory>
#include "lyricswidget.h"
#include "audioengine.h"
#include "track.h"

class LyricsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LyricsWindow(QWidget *parent = nullptr);
    ~LyricsWindow();

    void setAudioEngine(AudioEngine* engine);
    void onTrackChanged(std::shared_ptr<Track> track);
    void onPositionChanged(int seconds);
    void updateLyrics(const QString& trackId, const QString& lyrics, const QJsonArray& syncedLyrics);

protected:
    void closeEvent(QCloseEvent* event) override;

signals:
    void closed();
    void debugLog(const QString& message);

private:
    LyricsWidget* m_lyricsWidget;
    AudioEngine* m_audioEngine;
    QString m_currentTrackId;
};

#endif // LYRICSWINDOW_H
