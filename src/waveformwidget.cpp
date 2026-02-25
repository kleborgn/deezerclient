#include "waveformwidget.h"
#include <QPainter>
#include <QPalette>
#include <QMouseEvent>

static constexpr int BAR_WIDTH = 3;
static constexpr int BAR_GAP   = 1;
static constexpr int BAR_STEP  = BAR_WIDTH + BAR_GAP;

// Deezer-themed palette
static const QColor COLOR_PLAYED(162, 56, 255);        // #A238FF  Deezer purple
static const QColor DEFAULT_UNPLAYED(140, 140, 140);   // medium gray
static const QColor COLOR_PLAYHEAD(162, 56, 255);      // #A238FF
static const QColor COLOR_HOVER(255, 255, 255, 100);

WaveformWidget::WaveformWidget(QWidget *parent)
    : QWidget(parent)
    , m_position(0.0)
    , m_dragPosition(0.0)
    , m_dragging(false)
    , m_hovering(false)
    , m_hoverPosition(0.0)
    , m_unplayedColor(DEFAULT_UNPLAYED)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(64);
}

// ── public setters ──────────────────────────────────────────────────

void WaveformWidget::setPeaks(const QVector<float>& peaks)
{
    m_peaks = peaks;
    update();
}

void WaveformWidget::setPosition(double position)
{
    if (!m_dragging) {
        m_position = qBound(0.0, position, 1.0);
        update();
    }
}

void WaveformWidget::setUnplayedColor(const QColor& color)
{
    m_unplayedColor = color.isValid() ? color : DEFAULT_UNPLAYED;
    update();
}

void WaveformWidget::clear()
{
    m_peaks.clear();
    m_position = 0.0;
    m_dragging = false;
    update();
}

// ── size hints ──────────────────────────────────────────────────────

QSize WaveformWidget::minimumSizeHint() const { return QSize(100, 48); }
QSize WaveformWidget::sizeHint()        const { return QSize(400, 64); }

// ── helpers ─────────────────────────────────────────────────────────

double WaveformWidget::positionFromX(int x) const
{
    return qBound(0.0, static_cast<double>(x) / width(), 1.0);
}

// ── painting ────────────────────────────────────────────────────────

void WaveformWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int w  = width();
    const int h  = height();
    const int cy = h / 2;              // vertical centre
    const int maxHalf = cy - 2;        // max bar half-height (2 px padding)

    // Background - transparent to blend with rest of UI
    p.fillRect(rect(), Qt::transparent);

    const double displayPos = m_dragging ? m_dragPosition : m_position;

    if (m_peaks.isEmpty()) {
        // ── no waveform yet: draw a thin progress bar ───────────
        const int barH = 4;
        if (displayPos > 0.0) {
            int px = static_cast<int>(displayPos * w);
            p.fillRect(0,  cy - barH / 2, px,     barH, COLOR_PLAYED);
            p.fillRect(px, cy - barH / 2, w - px, barH, m_unplayedColor);
        } else {
            p.fillRect(0, cy - barH / 2, w, barH, m_unplayedColor);
        }
    } else {
        // ── draw waveform bars ──────────────────────────────────
        const int numBars   = qMax(1, w / BAR_STEP);
        const int playedBar = static_cast<int>(displayPos * numBars);

        for (int i = 0; i < numBars; ++i) {
            // Map visible bar → peak index
            const float fIdx = static_cast<float>(i) * m_peaks.size() / numBars;
            const int   idx  = qBound(0, static_cast<int>(fIdx), m_peaks.size() - 1);

            float peak = m_peaks[idx];
            peak = qMax(peak, 0.03f);                       // minimum visible height

            const int halfH = qMax(1, static_cast<int>(peak * maxHalf));
            const int x     = i * BAR_STEP;
            const bool played = (i <= playedBar);
            const QColor color = played ? COLOR_PLAYED : m_unplayedColor;

            // Symmetric bar around centre line
            p.fillRect(x, cy - halfH, BAR_WIDTH, halfH * 2, color);
        }
    }

    // ── playhead line ───────────────────────────────────────────
    {
        const int px = static_cast<int>(displayPos * w);
        p.setPen(QPen(COLOR_PLAYHEAD, 2));
        p.drawLine(px, 0, px, h);
    }

    // ── hover indicator ─────────────────────────────────────────
    if (m_hovering && !m_dragging) {
        const int hx = static_cast<int>(m_hoverPosition * w);
        p.setPen(QPen(COLOR_HOVER, 1));
        p.drawLine(hx, 0, hx, h);
    }
}

// ── mouse interaction ───────────────────────────────────────────────

void WaveformWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging     = true;
        m_dragPosition = positionFromX(event->pos().x());
        update();
    }
}

void WaveformWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        m_dragPosition = positionFromX(event->pos().x());
    } else {
        m_hoverPosition = positionFromX(event->pos().x());
    }
    update();
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging  = false;
        m_position  = m_dragPosition;
        emit seekRequested(m_position);
        update();
    }
}

void WaveformWidget::enterEvent(QEnterEvent *)
{
    m_hovering = true;
    update();
}

void WaveformWidget::leaveEvent(QEvent *)
{
    m_hovering = false;
    update();
}
