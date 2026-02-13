#ifndef SPECTRUMWINDOW_H
#define SPECTRUMWINDOW_H

#include <QWidget>
#include <QCloseEvent>
#include "spectrumwidget.h"
#include "audioengine.h"

class SpectrumWindow : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrumWindow(QWidget *parent = nullptr);
    ~SpectrumWindow();

    void setAudioEngine(AudioEngine* engine);

protected:
    void closeEvent(QCloseEvent* event) override;

signals:
    void closed();

private:
    SpectrumWidget* m_spectrumWidget;
    AudioEngine* m_audioEngine;
};

#endif // SPECTRUMWINDOW_H
