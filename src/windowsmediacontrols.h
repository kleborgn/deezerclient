#ifndef WINDOWSMEDIACONTROLS_H
#define WINDOWSMEDIACONTROLS_H

#include <QObject>
#include <QString>
#include <QUrl>

class WindowsMediaControls : public QObject
{
    Q_OBJECT
public:
    explicit WindowsMediaControls(QObject *parent = nullptr);
    ~WindowsMediaControls();

    void setEnabled(bool enabled);
    void updateMetadata(const QString& title, const QString& artist, const QString& album, const QUrl& artUrl = QUrl());
    void updatePlaybackState(bool playing);

signals:
    void playRequested();
    void pauseRequested();
    void nextRequested();
    void previousRequested();

private:
    class Private;
    Private* d;
};

#endif // WINDOWSMEDIACONTROLS_H
