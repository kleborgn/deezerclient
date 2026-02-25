#include "queueheaderwidget.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmap>
#include <QFont>

QueueHeaderWidget::QueueHeaderWidget(QWidget *parent)
    : QWidget(parent)
{
    m_networkManager = new QNetworkAccessManager(this);
    setupUI();
}

void QueueHeaderWidget::setupUI()
{
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(12);

    // Small artwork thumbnail
    m_artLabel = new QLabel(this);
    m_artLabel->setFixedSize(90, 90);
    m_artLabel->setStyleSheet("border: 1px solid #444; background: #222; border-radius: 4px;");
    m_artLabel->setScaledContents(true);
    m_artLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_artLabel, 0, Qt::AlignTop);

    // Text info stack
    QVBoxLayout* textLayout = new QVBoxLayout();
    textLayout->setSpacing(2);

    // Artist name (bold, larger)
    m_artistLabel = new QLabel(this);
    m_artistLabel->setObjectName("qhArtist");
    QFont artistFont = m_artistLabel->font();
    artistFont.setPointSize(14);
    artistFont.setBold(true);
    m_artistLabel->setFont(artistFont);
    m_artistLabel->setAutoFillBackground(false);
    textLayout->addWidget(m_artistLabel);

    // Album title
    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName("qhTitle");
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(12);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setAutoFillBackground(false);
    textLayout->addWidget(m_titleLabel);

    // Stream info (format, bitrate, sample rate)
    m_streamInfoLabel = new QLabel(this);
    m_streamInfoLabel->setObjectName("qhDim");
    m_streamInfoLabel->setAutoFillBackground(false);
    m_streamInfoLabel->hide();
    textLayout->addWidget(m_streamInfoLabel);

    // Stats (track count, duration)
    m_statsLabel = new QLabel(this);
    m_statsLabel->setObjectName("qhDim2");
    m_statsLabel->setAutoFillBackground(false);
    textLayout->addWidget(m_statsLabel);

    // Scrobble count
    m_scrobbleLabel = new QLabel(this);
    m_scrobbleLabel->setObjectName("qhDim3");
    m_scrobbleLabel->setAutoFillBackground(false);
    m_scrobbleLabel->hide();
    textLayout->addWidget(m_scrobbleLabel);

    textLayout->addStretch();
    layout->addLayout(textLayout, 1);

    setStyleSheet(
        "QueueHeaderWidget { background: #1a1a1a; border-bottom: 1px solid #333; }"
        "QLabel { background: transparent; }"
        "#qhArtist { color: white; }"
        "#qhTitle { color: #ccc; }"
        "#qhDim, #qhDim2, #qhDim3 { color: #888; }"
    );
}

void QueueHeaderWidget::setAlbum(std::shared_ptr<Album> album)
{
    if (!album) return;

    m_artistLabel->setText(album->artist());

    // Title with year
    QString year = album->releaseDate().left(4);
    if (!year.isEmpty()) {
        m_titleLabel->setText(album->title() + "   " + year);
    } else {
        m_titleLabel->setText(album->title());
    }

    m_baseStats = QString("%1 Tracks | Time: %2")
                      .arg(album->trackCount())
                      .arg(formatDuration(album->totalDuration()));
    m_statsLabel->setText(m_baseStats);

    loadImage(album->coverUrl());
    show();
}

void QueueHeaderWidget::setPlaylist(std::shared_ptr<Playlist> playlist)
{
    if (!playlist) return;

    m_artistLabel->setText("Playlist");
    m_titleLabel->setText(playlist->title());

    m_baseStats = QString("%1 Tracks | Time: %2")
                      .arg(playlist->trackCount())
                      .arg(formatDuration(playlist->totalDuration()));
    m_statsLabel->setText(m_baseStats);

    loadImage(playlist->coverUrl());
    show();
}

void QueueHeaderWidget::clear()
{
    m_artistLabel->clear();
    m_titleLabel->clear();
    m_statsLabel->clear();
    m_streamInfoLabel->clear();
    m_streamInfoLabel->hide();
    m_scrobbleLabel->clear();
    m_scrobbleLabel->hide();
    m_artLabel->clear();
    m_baseStats.clear();
    hide();
}

void QueueHeaderWidget::setStreamInfo(const QString& info)
{
    m_streamInfoLabel->setText(info);
    m_streamInfoLabel->show();
}

QString QueueHeaderWidget::formatDuration(int seconds)
{
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = seconds % 60;

    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    } else {
        return QString("%1:%2")
            .arg(minutes)
            .arg(secs, 2, 10, QChar('0'));
    }
}

void QueueHeaderWidget::loadImage(const QString& url)
{
    if (url.isEmpty()) {
        m_artLabel->setText("No Art");
        return;
    }

    QNetworkReply* reply = m_networkManager->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            QPixmap pixmap;
            if (pixmap.loadFromData(reply->readAll())) {
                m_artLabel->setPixmap(pixmap);
            }
        } else {
            m_artLabel->setText("No Art");
        }
        reply->deleteLater();
    });
}

void QueueHeaderWidget::setDynamicColors(const QColor& bgColor, const QString& textColor, const QString& dimColor)
{
    setStyleSheet(QString(
        "QueueHeaderWidget { background: %1; border-bottom: 1px solid %2; }"
        "QLabel { background: transparent; }"
        "#qhArtist { color: %3; }"
        "#qhTitle { color: %4; }"
        "#qhDim, #qhDim2, #qhDim3 { color: %4; }"
    ).arg(bgColor.name(), bgColor.lighter(130).name(), textColor, dimColor));
}

void QueueHeaderWidget::resetDefaultColors()
{
    setStyleSheet(
        "QueueHeaderWidget { background: #1a1a1a; border-bottom: 1px solid #333; }"
        "QLabel { background: transparent; }"
        "#qhArtist { color: white; }"
        "#qhTitle { color: #ccc; }"
        "#qhDim, #qhDim2, #qhDim3 { color: #888; }"
    );
}

void QueueHeaderWidget::setAlbumScrobbleCount(int count)
{
    if (count >= 0) {
        m_scrobbleLabel->setText(QString::fromUtf8("\u266B") + QString(" %1 scrobbles").arg(count));
        m_scrobbleLabel->show();
    } else {
        m_scrobbleLabel->hide();
    }
}

void QueueHeaderWidget::updateStatsLine()
{
    m_statsLabel->setText(m_baseStats);
}
