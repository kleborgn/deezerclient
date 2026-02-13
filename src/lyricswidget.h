#ifndef LYRICSWIDGET_H
#define LYRICSWIDGET_H

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QVector>

struct LyricLine {
    QString text;
    int milliseconds;  // Timestamp in milliseconds
};

class LyricsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LyricsWidget(QWidget *parent = nullptr);

    void setLyrics(const QString& lyrics, const QJsonArray& syncedLyrics);
    void setPosition(int seconds);
    void clear();

signals:
    void debugLog(const QString& message);

private:
    void parsePlainTextLyrics(const QString& lyrics);
    void parseSyncedLyrics(const QJsonArray& syncedLyrics);
    void rebuildUI();
    void updateHighlight();
    int findCurrentLineIndex(int seconds) const;

    QScrollArea* m_scrollArea;
    QWidget* m_contentWidget;
    QVBoxLayout* m_contentLayout;
    QVector<QLabel*> m_lineLabels;
    QVector<LyricLine> m_lines;
    int m_currentLineIndex;
    bool m_hasSyncedLyrics;
};

#endif // LYRICSWIDGET_H
