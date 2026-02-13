#include "tracklistwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QContextMenuEvent>
#include <QMenu>
#include <QKeyEvent>
#include <QDropEvent>
#include <algorithm>

// Custom table widget with drag-and-drop support
class DraggableTableWidget : public QTableWidget {
    Q_OBJECT
public:
    explicit DraggableTableWidget(QWidget* parent = nullptr)
        : QTableWidget(parent), m_dragStartRow(-1) {}

signals:
    void rowMoved(int fromRow, int toRow);

protected:
    void startDrag(Qt::DropActions supportedActions) override {
        m_dragStartRow = currentRow();
        QTableWidget::startDrag(supportedActions);
    }

    void dropEvent(QDropEvent* event) override {
        int fromRow = m_dragStartRow;
        if (fromRow < 0) {
            event->ignore();
            return;
        }

        // Find the drop position
        QTableWidgetItem* item = itemAt(event->position().toPoint());
        int toRow = item ? item->row() : rowCount() - 1;

        // Accept the event to prevent Qt's default behavior
        event->accept();

        // Reset drag tracking
        m_dragStartRow = -1;

        // Emit signal to update backend (UI will refresh via queueChanged signal)
        if (fromRow != toRow && fromRow >= 0 && toRow >= 0) {
            emit rowMoved(fromRow, toRow);
        }
    }

private:
    int m_dragStartRow;
};

TrackListWidget::TrackListWidget(QWidget *parent)
    : QWidget(parent)
    , m_deezerAPI(nullptr)
{
    setupUI();
}

TrackListWidget::~TrackListWidget()
{
    // QTableWidget is a child widget, will be deleted automatically
}

void TrackListWidget::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // Search bar
    QHBoxLayout* searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search for tracks, artists, albums...");
    m_searchButton = new QPushButton("Search", this);

    searchLayout->addWidget(m_searchEdit);
    searchLayout->addWidget(m_searchButton);

    // Track table (use custom draggable widget)
    m_trackTable = new DraggableTableWidget(this);
    m_trackTable->setColumnCount(5);
    m_trackTable->setHorizontalHeaderLabels({"Title", "Artist", "Album", "Duration", "Scrobbles"});
    m_trackTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_trackTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_trackTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_trackTable->horizontalHeader()->setStretchLastSection(true);
    m_trackTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_trackTable->verticalHeader()->setVisible(false);

    // Enable drag-and-drop (initially disabled, enabled in setMode)
    m_trackTable->setDragEnabled(false);
    m_trackTable->setAcceptDrops(false);
    m_trackTable->setDragDropMode(QAbstractItemView::InternalMove);
    m_trackTable->setDefaultDropAction(Qt::MoveAction);

    // Install event filter for keyboard shortcuts
    m_trackTable->installEventFilter(this);

    // Add to main layout
    mainLayout->addLayout(searchLayout);
    mainLayout->addWidget(m_trackTable);

    // Connect signals
    connect(m_searchButton, &QPushButton::clicked, this, &TrackListWidget::onSearchClicked);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &TrackListWidget::onSearchClicked);
    connect(m_trackTable, &QTableWidget::cellDoubleClicked, this, &TrackListWidget::onTableDoubleClicked);
    connect(m_trackTable, &QTableWidget::cellClicked, this, &TrackListWidget::onCellClicked);
    connect(m_trackTable, &QTableWidget::itemSelectionChanged, this, &TrackListWidget::onSelectionChanged);

    // Connect drag-and-drop signal
    connect(static_cast<DraggableTableWidget*>(m_trackTable),
            &DraggableTableWidget::rowMoved,
            this, &TrackListWidget::moveRequested);
}

void TrackListWidget::setDeezerAPI(DeezerAPI* api)
{
    m_deezerAPI = api;
    // Connection to search signals is managed by setSearchVisible()
    // to ensure we only receive search results when this widget's search is enabled
}

void TrackListWidget::setTracks(const QList<std::shared_ptr<Track>>& tracks)
{
    m_tracks = tracks;
    populateTable();
}

void TrackListWidget::clearTracks()
{
    m_tracks.clear();
    m_trackTable->setRowCount(0);
}

void TrackListWidget::setSearchVisible(bool visible)
{
    m_searchEdit->setVisible(visible);
    m_searchButton->setVisible(visible);

    // Only connect to search signals if search is enabled for this widget
    if (m_deezerAPI) {
        if (visible) {
            // Disconnect first to avoid duplicate connections
            disconnect(m_deezerAPI, &DeezerAPI::searchTracksFound, this, &TrackListWidget::onTracksFound);
            connect(m_deezerAPI, &DeezerAPI::searchTracksFound, this, &TrackListWidget::onTracksFound);
        } else {
            // Disconnect when search is hidden to avoid receiving search results
            disconnect(m_deezerAPI, &DeezerAPI::searchTracksFound, this, &TrackListWidget::onTracksFound);
        }
    }
}

void TrackListWidget::onSearchClicked()
{
    QString query = m_searchEdit->text().trimmed();
    if (!query.isEmpty() && m_deezerAPI) {
        // Use context-aware search - pass 'this' as context
        m_deezerAPI->searchTracksWithContext(query, 50, this);
    }
}

void TrackListWidget::onTracksFound(QList<std::shared_ptr<Track>> tracks, void* sender)
{
    // Only process results if they were requested by this widget
    if (sender && sender != this) {
        return;
    }
    setTracks(tracks);
}

void TrackListWidget::onTableDoubleClicked(int row, int column)
{
    if (row >= 0 && row < m_tracks.size()) {
        emit trackDoubleClicked(m_tracks[row]);
    }
}

void TrackListWidget::onCellClicked(int row, int column)
{
    // Heart column is always the last column
    int heartCol = m_trackTable->columnCount() - 1;
    if (column != heartCol) return;
    if (row < 0 || row >= m_tracks.size()) return;

    auto track = m_tracks[row];
    bool newFavorite = !track->isFavorite();
    track->setFavorite(newFavorite);

    // Update heart display
    QTableWidgetItem* item = m_trackTable->item(row, heartCol);
    if (item) {
        item->setText(newFavorite
            ? QString::fromUtf8("\u2665")   // ♥ filled
            : QString::fromUtf8("\u2661")); // ♡ empty
        item->setForeground(newFavorite
            ? QBrush(QColor(220, 60, 60))
            : QBrush(QColor(180, 180, 180)));
    }

    emit favoriteToggled(track, newFavorite);
}

void TrackListWidget::onSelectionChanged()
{
    QList<std::shared_ptr<Track>> selectedTracks;
    
    for (QTableWidgetItem* item : m_trackTable->selectedItems()) {
        int row = item->row();
        if (row >= 0 && row < m_tracks.size()) {
            if (!selectedTracks.contains(m_tracks[row])) {
                selectedTracks.append(m_tracks[row]);
            }
        }
    }
    
    if (!selectedTracks.isEmpty()) {
        emit tracksSelected(selectedTracks);
    }
}

void TrackListWidget::setCurrentTrackId(const QString& id)
{
    for (int i = 0; i < m_trackTable->rowCount(); ++i) {
        bool isCurrent = (i < m_tracks.size() && m_tracks[i]->id() == id);

        for (int col = 0; col < m_trackTable->columnCount(); ++col) {
            QTableWidgetItem* item = m_trackTable->item(i, col);
            if (!item) continue;

            QFont font = item->font();
            font.setBold(isCurrent);
            item->setFont(font);

            if (isCurrent) {
                item->setBackground(QBrush(QColor(60, 60, 100)));
            } else {
                item->setBackground(Qt::transparent);
            }
        }

        // In QueueMode, update # column with play indicator
        if (m_mode == QueueMode) {
            QTableWidgetItem* numItem = m_trackTable->item(i, 0);
            if (numItem) {
                if (isCurrent) {
                    numItem->setText(QString::fromUtf8("\u25B6"));  // ▶
                } else {
                    numItem->setText(QString("%1.").arg(i + 1, 2, 10, QChar('0')));
                }
            }
        }
    }
}

void TrackListWidget::populateTable()
{
    m_trackTable->setRowCount(0);

    if (m_mode == QueueMode) {
        // Simplified queue columns: #, Title, Scrobbles, Duration, ♡
        m_trackTable->setColumnCount(5);
        m_trackTable->setHorizontalHeaderLabels({"#", "Title", "Scrobbles", "Duration", ""});
        m_trackTable->horizontalHeader()->setVisible(false);

        // Column sizing
        m_trackTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
        m_trackTable->setColumnWidth(0, 40);
        m_trackTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_trackTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
        m_trackTable->setColumnWidth(2, 60);
        m_trackTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
        m_trackTable->setColumnWidth(3, 60);
        m_trackTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
        m_trackTable->setColumnWidth(4, 30);

        for (int i = 0; i < m_tracks.size(); ++i) {
            auto track = m_tracks[i];
            m_trackTable->insertRow(i);

            // Track number
            QString numStr = QString("%1.").arg(i + 1, 2, 10, QChar('0'));
            QTableWidgetItem* numItem = new QTableWidgetItem(numStr);
            numItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_trackTable->setItem(i, 0, numItem);

            m_trackTable->setItem(i, 1, new QTableWidgetItem(track->title()));

            // Scrobble count
            QString scrobbleText = track->hasScrobbleData()
                ? QString::number(track->userScrobbleCount())
                : QString();
            QTableWidgetItem* scrobbleItem = new QTableWidgetItem(scrobbleText);
            scrobbleItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_trackTable->setItem(i, 2, scrobbleItem);

            QTableWidgetItem* durItem = new QTableWidgetItem(track->durationString());
            durItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_trackTable->setItem(i, 3, durItem);

            // Favorite heart
            QString heartText = track->isFavorite()
                ? QString::fromUtf8("\u2665")   // ♥ filled
                : QString::fromUtf8("\u2661");  // ♡ empty
            QTableWidgetItem* heartItem = new QTableWidgetItem(heartText);
            heartItem->setTextAlignment(Qt::AlignCenter);
            if (track->isFavorite()) {
                heartItem->setForeground(QBrush(QColor(220, 60, 60)));
            }
            m_trackTable->setItem(i, 4, heartItem);
        }
    } else {
        // Library mode: full 6-column layout
        m_trackTable->setColumnCount(6);
        m_trackTable->setHorizontalHeaderLabels({"Title", "Artist", "Album", "Duration", "Scrobbles", ""});
        m_trackTable->horizontalHeader()->setVisible(true);
        m_trackTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_trackTable->horizontalHeader()->setStretchLastSection(false);
        m_trackTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
        m_trackTable->setColumnWidth(5, 30);

        for (int i = 0; i < m_tracks.size(); ++i) {
            auto track = m_tracks[i];

            m_trackTable->insertRow(i);
            m_trackTable->setItem(i, 0, new QTableWidgetItem(track->title()));
            m_trackTable->setItem(i, 1, new QTableWidgetItem(track->artist()));
            m_trackTable->setItem(i, 2, new QTableWidgetItem(track->album()));
            m_trackTable->setItem(i, 3, new QTableWidgetItem(track->durationString()));

            QString scrobbleText = track->hasScrobbleData()
                ? QString::number(track->userScrobbleCount())
                : QString::fromUtf8("\u2014");
            m_trackTable->setItem(i, 4, new QTableWidgetItem(scrobbleText));

            // Favorite heart
            QString heartText = track->isFavorite()
                ? QString::fromUtf8("\u2665")   // ♥ filled
                : QString::fromUtf8("\u2661");  // ♡ empty
            QTableWidgetItem* heartItem = new QTableWidgetItem(heartText);
            heartItem->setTextAlignment(Qt::AlignCenter);
            if (track->isFavorite()) {
                heartItem->setForeground(QBrush(QColor(220, 60, 60)));
            }
            m_trackTable->setItem(i, 5, heartItem);
        }
    }
}

void TrackListWidget::setMode(Mode mode)
{
    m_mode = mode;

    // Only enable drag-and-drop in queue mode
    if (mode == QueueMode) {
        m_trackTable->setDragEnabled(true);
        m_trackTable->setAcceptDrops(true);
    } else {
        m_trackTable->setDragEnabled(false);
        m_trackTable->setAcceptDrops(false);
    }

    // Re-populate with correct columns if tracks are loaded
    if (!m_tracks.isEmpty()) {
        populateTable();
    }
}

void TrackListWidget::updateTrackScrobbleCount(int index)
{
    if (index < 0 || index >= m_tracks.size()) return;

    auto track = m_tracks[index];
    QString text = track->hasScrobbleData()
        ? QString::number(track->userScrobbleCount())
        : (m_mode == QueueMode ? QString() : QString::fromUtf8("\u2014"));

    int scrobbleCol = (m_mode == QueueMode) ? 2 : 4;
    QTableWidgetItem* item = m_trackTable->item(index, scrobbleCol);
    if (item) {
        item->setText(text);
    }
}

bool TrackListWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_trackTable && event->type() == QEvent::KeyPress &&
        m_mode == QueueMode) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);

        // Delete key - remove selected tracks
        if (keyEvent->key() == Qt::Key_Delete) {
            QList<int> selectedRows;
            for (QTableWidgetItem* item : m_trackTable->selectedItems()) {
                int row = item->row();
                if (!selectedRows.contains(row)) {
                    selectedRows.append(row);
                }
            }

            if (!selectedRows.isEmpty()) {
                if (selectedRows.size() == 1) {
                    emit removeRequested(selectedRows.first());
                } else {
                    emit removeMultipleRequested(selectedRows);
                }
            }
            return true;
        }

        // Ctrl+Up - move track up
        if (keyEvent->key() == Qt::Key_Up &&
            (keyEvent->modifiers() & Qt::ControlModifier)) {
            int row = m_trackTable->currentRow();
            if (row > 0) {
                emit moveRequested(row, row - 1);
                m_trackTable->selectRow(row - 1);  // Follow selection
            }
            return true;
        }

        // Ctrl+Down - move track down
        if (keyEvent->key() == Qt::Key_Down &&
            (keyEvent->modifiers() & Qt::ControlModifier)) {
            int row = m_trackTable->currentRow();
            if (row >= 0 && row < m_trackTable->rowCount() - 1) {
                emit moveRequested(row, row + 1);
                m_trackTable->selectRow(row + 1);  // Follow selection
            }
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

void TrackListWidget::contextMenuEvent(QContextMenuEvent* event)
{
    QTableWidgetItem* item = m_trackTable->itemAt(m_trackTable->viewport()->mapFrom(this, event->pos()));
    if (!item)
        return;

    int clickedRow = item->row();
    QList<int> selectedRows;

    // Collect all selected rows
    for (QTableWidgetItem* selectedItem : m_trackTable->selectedItems()) {
        int row = selectedItem->row();
        if (!selectedRows.contains(row)) {
            selectedRows.append(row);
        }
    }

    // If clicked row not in selection, use only clicked row
    if (!selectedRows.contains(clickedRow)) {
        selectedRows.clear();
        selectedRows.append(clickedRow);
        m_trackTable->selectRow(clickedRow);
    }

    // Only show context menu in library mode
    if (m_mode != LibraryMode)
        return;

    // Gather selected tracks
    QList<std::shared_ptr<Track>> selectedTracks;
    for (int row : selectedRows) {
        if (row >= 0 && row < m_tracks.size()) {
            selectedTracks.append(m_tracks[row]);
        }
    }
    if (selectedTracks.isEmpty())
        return;

    // Create context menu
    QMenu contextMenu(this);

    QString trackText = (selectedTracks.size() == 1)
        ? "Track"
        : QString("%1 Tracks").arg(selectedTracks.size());

    QAction* playNextAction = contextMenu.addAction("Play Next");
    QAction* addToQueueAction = contextMenu.addAction(
        QString("Add %1 to Queue").arg(trackText)
    );

    connect(playNextAction, &QAction::triggered, [this, selectedTracks]() {
        emit playNextRequested(selectedTracks);
    });

    connect(addToQueueAction, &QAction::triggered, [this, selectedTracks]() {
        emit addToQueueRequested(selectedTracks);
    });

    contextMenu.exec(event->globalPos());
}

// Include moc file for DraggableTableWidget
#include "tracklistwidget.moc"
