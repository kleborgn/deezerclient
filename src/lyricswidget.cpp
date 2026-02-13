#include "lyricswidget.h"
#include <QFont>
#include <QJsonObject>

LyricsWidget::LyricsWidget(QWidget *parent)
    : QWidget(parent)
    , m_currentLineIndex(-1)
    , m_hasSyncedLyrics(false)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet("QScrollArea { background: #1a1a1a; border: none; }");

    m_contentWidget = new QWidget();
    m_contentLayout = new QVBoxLayout(m_contentWidget);
    m_contentLayout->setContentsMargins(20, 20, 20, 20);
    m_contentLayout->setSpacing(8);
    m_contentLayout->addStretch();

    m_scrollArea->setWidget(m_contentWidget);
    mainLayout->addWidget(m_scrollArea);

    // Default message
    QLabel* emptyLabel = new QLabel("No lyrics loaded", this);
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setStyleSheet("color: #666; font-size: 14px;");
    m_contentLayout->addWidget(emptyLabel);
}

void LyricsWidget::setLyrics(const QString& lyrics, const QJsonArray& syncedLyrics)
{
    clear();

    // Try synced lyrics first
    if (!syncedLyrics.isEmpty()) {
        parseSyncedLyrics(syncedLyrics);
        m_hasSyncedLyrics = true;
        emit debugLog(QString("[LyricsWidget] Parsed %1 synced lines").arg(m_lines.size()));
        if (!m_lines.isEmpty()) {
            emit debugLog(QString("[LyricsWidget] First line: '%1' at %2ms")
                         .arg(m_lines.first().text)
                         .arg(m_lines.first().milliseconds));
            emit debugLog(QString("[LyricsWidget] Last line: '%1' at %2ms")
                         .arg(m_lines.last().text)
                         .arg(m_lines.last().milliseconds));
        }
    }
    // Fall back to plain text
    else if (!lyrics.isEmpty()) {
        parsePlainTextLyrics(lyrics);
        m_hasSyncedLyrics = false;
        emit debugLog(QString("[LyricsWidget] Parsed %1 plain text lines").arg(m_lines.size()));
    }
    // No lyrics available
    else {
        QLabel* noLyricsLabel = new QLabel("Lyrics not available for this track", m_contentWidget);
        noLyricsLabel->setAlignment(Qt::AlignCenter);
        noLyricsLabel->setStyleSheet("color: #666; font-size: 14px;");
        m_contentLayout->insertWidget(0, noLyricsLabel);
        m_lineLabels.append(noLyricsLabel);
        return;
    }

    rebuildUI();
}

void LyricsWidget::parsePlainTextLyrics(const QString& lyrics)
{
    QStringList lines = lyrics.split('\n');
    for (const QString& line : lines) {
        LyricLine lyricLine;
        lyricLine.text = line.trimmed();
        lyricLine.milliseconds = -1;  // No timestamp
        if (!lyricLine.text.isEmpty()) {
            m_lines.append(lyricLine);
        }
    }
}

void LyricsWidget::parseSyncedLyrics(const QJsonArray& syncedLyrics)
{
    for (const QJsonValue& val : syncedLyrics) {
        if (!val.isObject()) continue;

        QJsonObject lineObj = val.toObject();
        LyricLine lyricLine;

        // Extract text - try multiple field names
        if (lineObj.contains("line")) {
            lyricLine.text = lineObj["line"].toString();
        } else if (lineObj.contains("text")) {
            lyricLine.text = lineObj["text"].toString();
        } else if (lineObj.contains("LYRICS_TEXT")) {
            lyricLine.text = lineObj["LYRICS_TEXT"].toString();
        }

        // Extract timestamp - try multiple field names
        // Note: Deezer API returns milliseconds as a STRING, not a number
        if (lineObj.contains("milliseconds")) {
            lyricLine.milliseconds = lineObj["milliseconds"].toString().toInt();
        } else if (lineObj.contains("lrc_timestamp")) {
            // Parse LRC timestamp format "mm:ss.xx"
            QString lrcTime = lineObj["lrc_timestamp"].toString();
            QStringList parts = lrcTime.split(':');
            if (parts.size() == 2) {
                int minutes = parts[0].toInt();
                double seconds = parts[1].toDouble();
                lyricLine.milliseconds = (minutes * 60 * 1000) + (seconds * 1000);
            }
        } else if (lineObj.contains("time")) {
            lyricLine.milliseconds = lineObj["time"].toString().toInt();
        } else if (lineObj.contains("duration_ms")) {
            lyricLine.milliseconds = lineObj["duration_ms"].toString().toInt();
        }

        if (!lyricLine.text.isEmpty()) {
            m_lines.append(lyricLine);
        }
    }
}

void LyricsWidget::rebuildUI()
{
    // Clear existing labels
    qDeleteAll(m_lineLabels);
    m_lineLabels.clear();

    // Create label for each line
    for (int i = 0; i < m_lines.size(); ++i) {
        QLabel* lineLabel = new QLabel(m_lines[i].text, m_contentWidget);
        lineLabel->setWordWrap(true);
        lineLabel->setAlignment(Qt::AlignCenter);

        QFont font = lineLabel->font();
        font.setPointSize(13);
        lineLabel->setFont(font);

        lineLabel->setStyleSheet("color: #888; padding: 4px;");

        m_contentLayout->insertWidget(i, lineLabel);
        m_lineLabels.append(lineLabel);
    }
}

void LyricsWidget::setPosition(int seconds)
{
    if (m_lines.isEmpty()) return;

    int newLineIndex = findCurrentLineIndex(seconds);

    if (newLineIndex != m_currentLineIndex) {
        m_currentLineIndex = newLineIndex;
        emit debugLog(QString("[LyricsWidget] Position %1s -> line %2/%3: '%4'")
                     .arg(seconds)
                     .arg(m_currentLineIndex)
                     .arg(m_lines.size() - 1)
                     .arg(m_currentLineIndex >= 0 && m_currentLineIndex < m_lines.size()
                          ? m_lines[m_currentLineIndex].text : "none"));
        updateHighlight();

        // Auto-scroll to current line
        if (m_currentLineIndex >= 0 && m_currentLineIndex < m_lineLabels.size()) {
            QLabel* currentLabel = m_lineLabels[m_currentLineIndex];
            m_scrollArea->ensureWidgetVisible(currentLabel, 0, m_scrollArea->height() / 2);
        }
    }
}

int LyricsWidget::findCurrentLineIndex(int seconds) const
{
    if (!m_hasSyncedLyrics) {
        // For plain text lyrics, don't highlight
        return -1;
    }

    int milliseconds = seconds * 1000;
    int currentIndex = -1;

    for (int i = 0; i < m_lines.size(); ++i) {
        if (m_lines[i].milliseconds <= milliseconds) {
            currentIndex = i;
        } else {
            break;
        }
    }

    return currentIndex;
}

void LyricsWidget::updateHighlight()
{
    // Reset all labels to default style
    for (int i = 0; i < m_lineLabels.size(); ++i) {
        if (i == m_currentLineIndex) {
            // Highlight current line
            m_lineLabels[i]->setStyleSheet(
                "color: #ffffff; "
                "background: #333; "
                "border-radius: 4px; "
                "padding: 8px; "
                "font-weight: bold;"
            );
        } else {
            // Dim non-current lines
            m_lineLabels[i]->setStyleSheet("color: #888; padding: 4px;");
        }
    }
}

void LyricsWidget::clear()
{
    // Clear all labels
    qDeleteAll(m_lineLabels);
    m_lineLabels.clear();
    m_lines.clear();
    m_currentLineIndex = -1;
    m_hasSyncedLyrics = false;
}
