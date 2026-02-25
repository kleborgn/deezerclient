#include "recentwidget.h"
#include "deezerapi.h"

#include <QVBoxLayout>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QPixmap>
#include <QResizeEvent>
#include <QtMath>

static const int MAX_ITEMS = 11;
static const int MIN_COVER = 120;
static const int TEXT_HEIGHT = 40;  // space below cover for title/artist text

RecentWidget::RecentWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_listWidget = new QListWidget(this);
    m_listWidget->setViewMode(QListView::IconMode);
    m_listWidget->setWrapping(true);
    m_listWidget->setResizeMode(QListView::Adjust);
    m_listWidget->setMovement(QListView::Static);
    m_listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listWidget->setSpacing(8);
    m_listWidget->setUniformItemSizes(true);
    m_listWidget->setWordWrap(true);
    m_listWidget->setTextElideMode(Qt::ElideRight);
    layout->addWidget(m_listWidget);

    connect(m_listWidget, &QListWidget::itemDoubleClicked,
            this, &RecentWidget::onItemDoubleClicked);

    m_imageManager = new QNetworkAccessManager(this);
    connect(m_imageManager, &QNetworkAccessManager::finished,
            this, &RecentWidget::onImageDownloaded);
}

void RecentWidget::setDeezerAPI(DeezerAPI* api)
{
    m_deezerAPI = api;
    if (m_deezerAPI) {
        connect(m_deezerAPI, &DeezerAPI::recentlyPlayedReceived,
                this, &RecentWidget::onRecentlyPlayedReceived);
    }
}

void RecentWidget::refresh()
{
    if (m_deezerAPI)
        m_deezerAPI->getRecentlyPlayed();
}

QString RecentWidget::coverUrl(const QString& type, const QString& pictureId)
{
    if (pictureId.isEmpty()) return QString();
    // Request large images; we scale down in onImageDownloaded
    return QString("https://e-cdns-images.dzcdn.net/images/%1/%2/500x500-000000-80-0-0.jpg")
        .arg(type, pictureId);
}

int RecentWidget::computeCoverSize() const
{
    int spacing = m_listWidget->spacing();
    int availW = m_listWidget->viewport()->width();
    int availH = m_listWidget->viewport()->height();
    int count = m_listWidget->count() > 0 ? m_listWidget->count() : MAX_ITEMS;

    if (availW <= 0 || availH <= 0)
        return MIN_COVER;

    // Try column counts and pick the one that fills the viewport best.
    // For each column count, compute the cover that fits width, then check
    // if all rows fit vertically. Prefer fewer columns (bigger covers).
    int bestCover = MIN_COVER;
    for (int cols = 2; cols <= qMin(count, 8); ++cols) {
        int coverW = availW / cols - spacing;
        if (coverW < MIN_COVER) continue;

        int rows = (count + cols - 1) / cols;
        int totalH = rows * (coverW + TEXT_HEIGHT) + (rows - 1) * spacing;

        if (totalH <= availH) {
            // Fits vertically — use this (fewer cols = bigger covers)
            bestCover = coverW;
            break;
        }
    }

    // If nothing fits without scrolling, use width-based sizing
    // with enough columns to keep covers reasonable
    if (bestCover <= MIN_COVER) {
        for (int cols = 2; cols <= qMin(count, 8); ++cols) {
            int coverW = availW / cols - spacing;
            if (coverW >= MIN_COVER) {
                bestCover = coverW;
                break;
            }
        }
    }

    return bestCover;
}

void RecentWidget::updateGridSize()
{
    int cover = computeCoverSize();
    int spacing = m_listWidget->spacing();
    int availW = m_listWidget->viewport()->width();
    int cols = qMax(1, (availW + spacing) / (cover + spacing));
    int gridW = availW / cols;  // distribute full width evenly across columns

    m_listWidget->setIconSize(QSize(cover, cover));
    m_listWidget->setGridSize(QSize(gridW, cover + TEXT_HEIGHT));

    // Update size hints on existing items
    for (int i = 0; i < m_listWidget->count(); ++i) {
        QListWidgetItem* item = m_listWidget->item(i);
        if (item)
            item->setSizeHint(QSize(gridW, cover + TEXT_HEIGHT));
    }

    // Re-scale cached pixmaps to new size
    for (int i = 0; i < m_listWidget->count(); ++i) {
        QListWidgetItem* item = m_listWidget->item(i);
        if (!item) continue;
        int idx = item->data(Qt::UserRole + 2).toInt();
        if (idx >= 0 && idx < m_originalPixmaps.size() && !m_originalPixmaps[idx].isNull()) {
            item->setIcon(QIcon(m_originalPixmaps[idx].scaled(cover, cover, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
    }
}

void RecentWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateGridSize();
}

void RecentWidget::onRecentlyPlayedReceived(const QJsonArray& items)
{
    m_listWidget->clear();
    m_pendingImages.clear();
    m_originalPixmaps.clear();

    int cover = computeCoverSize();
    int spacing = m_listWidget->spacing();
    int availW = m_listWidget->viewport()->width();
    int cols = qMax(1, (availW + spacing) / (cover + spacing));
    int gridW = availW / cols;

    m_listWidget->setIconSize(QSize(cover, cover));
    m_listWidget->setGridSize(QSize(gridW, cover + TEXT_HEIGHT));

    int count = 0;
    for (const QJsonValue& val : items) {
        if (count >= MAX_ITEMS) break;

        QJsonObject wrapper = val.toObject();
        // Items are wrapped: {__TYPE__, data: {...}, timestamp}
        QJsonObject obj = wrapper["data"].toObject();
        QString type = obj["__TYPE__"].toString();

        QString title;
        QString subtitle;
        QString itemId;
        QString itemType;
        QString imageUrl;

        if (type == "album") {
            itemId = QString::number(obj["ALB_ID"].toVariant().toLongLong());
            title = obj["ALB_TITLE"].toString();
            subtitle = obj["ART_NAME"].toString();
            itemType = "album";
            imageUrl = coverUrl("cover", obj["ALB_PICTURE"].toString());
        } else if (type == "playlist") {
            itemId = QString::number(obj["PLAYLIST_ID"].toVariant().toLongLong());
            title = obj["TITLE"].toString();
            itemType = "playlist";
            imageUrl = coverUrl("playlist", obj["PLAYLIST_PICTURE"].toString());
        } else {
            emit debugLog(QString("[RecentWidget] Skipping item type '%1', keys: %2")
                .arg(type, QStringList(obj.keys()).join(", ")));
            continue;
        }

        QString displayText = subtitle.isEmpty() ? title : title + "\n" + subtitle;
        auto* item = new QListWidgetItem(displayText);
        item->setData(Qt::UserRole, itemId);
        item->setData(Qt::UserRole + 1, itemType);
        item->setData(Qt::UserRole + 2, count);  // index into m_originalPixmaps
        item->setSizeHint(QSize(gridW, cover + TEXT_HEIGHT));
        m_listWidget->addItem(item);
        m_originalPixmaps.append(QPixmap());  // placeholder

        // Download cover art
        if (!imageUrl.isEmpty()) {
            QNetworkReply* reply = m_imageManager->get(QNetworkRequest(QUrl(imageUrl)));
            m_pendingImages[reply] = count;
        }

        count++;
    }

    emit debugLog(QString("[RecentWidget] Displaying %1 recently played items").arg(count));
}

void RecentWidget::onItemDoubleClicked(QListWidgetItem* item)
{
    if (!item) return;
    QString id = item->data(Qt::UserRole).toString();
    QString type = item->data(Qt::UserRole + 1).toString();

    if (type == "album")
        emit albumDoubleClicked(id);
    else if (type == "playlist")
        emit playlistDoubleClicked(id);
}

void RecentWidget::onImageDownloaded(QNetworkReply* reply)
{
    reply->deleteLater();

    if (!m_pendingImages.contains(reply))
        return;

    int row = m_pendingImages.take(reply);

    if (reply->error() != QNetworkReply::NoError)
        return;

    QPixmap pixmap;
    if (!pixmap.loadFromData(reply->readAll()))
        return;

    // Store original for rescaling on resize
    if (row >= 0 && row < m_originalPixmaps.size())
        m_originalPixmaps[row] = pixmap;

    int cover = computeCoverSize();
    QListWidgetItem* item = m_listWidget->item(row);
    if (item)
        item->setIcon(QIcon(pixmap.scaled(cover, cover, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}
