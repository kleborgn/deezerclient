#ifndef TRACKLISTWIDGET_H
#define TRACKLISTWIDGET_H

#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <memory>
#include "track.h"
#include "deezerapi.h"

class TrackListWidget : public QWidget
{
    Q_OBJECT

public:
    enum Mode {
        QueueMode,    // Enable queue management features
        LibraryMode   // Enable "Add to Queue" features
    };

    explicit TrackListWidget(QWidget *parent = nullptr);

    void setDeezerAPI(DeezerAPI* api);
    void setTracks(const QList<std::shared_ptr<Track>>& tracks);
    void clearTracks();
    void setSearchVisible(bool visible);
    void setCurrentTrackId(const QString& id);
    void setHighlightColor(const QColor& color);
    void setHoverColor(const QColor& color);
    void setMode(Mode mode);
    void updateTrackScrobbleCount(int index);  // Update scrobble count for single track
    const QList<std::shared_ptr<Track>>& tracks() const { return m_tracks; }
    ~TrackListWidget();  // Need destructor to clean up network manager

signals:
    void trackDoubleClicked(std::shared_ptr<Track> track);
    void tracksSelected(QList<std::shared_ptr<Track>> tracks);

    // Queue mode signals
    void moveRequested(int fromIndex, int toIndex);
    void removeRequested(int index);
    void removeMultipleRequested(QList<int> indices);

    // Library mode signals
    void addToQueueRequested(QList<std::shared_ptr<Track>> tracks);
    void playNextRequested(QList<std::shared_ptr<Track>> tracks);

    // Favorite signal
    void favoriteToggled(std::shared_ptr<Track> track, bool isFavorite);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private slots:
    void onSearchClicked();
    void onTracksFound(QList<std::shared_ptr<Track>> tracks, void* sender = nullptr);
    void onTableDoubleClicked(int row, int column);
    void onCellClicked(int row, int column);
    void onSelectionChanged();
    void onCellEntered(int row, int column);

private:
    void setupUI();
    void populateTable();
    
    DeezerAPI* m_deezerAPI;  // Shared API for all operations
    QList<std::shared_ptr<Track>> m_tracks;

    QLineEdit* m_searchEdit;
    QPushButton* m_searchButton;
    QTableWidget* m_trackTable;

    Mode m_mode = LibraryMode;
    QColor m_highlightColor = QColor(60, 60, 100);
    QColor m_hoverColor = QColor(50, 50, 70);
    QString m_currentTrackId;
    int m_hoveredRow = -1;
};

#endif // TRACKLISTWIDGET_H
