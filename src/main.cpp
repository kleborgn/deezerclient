#include <QApplication>
#include "mainwindow.h"
#include "deezerapi.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Set application metadata
    QApplication::setApplicationName("Deezer Client");
    QApplication::setApplicationVersion("1.0.0");
    QApplication::setOrganizationName("DeezerClient");

    // OPTIONAL: Set Deezer mobile API key if you have it (for email/password login)
    // DeezerAPI::setApiKey("YOUR_MOBILE_API_KEY");
    //
    // OPTIONAL: Set TRACK_XOR_KEY for full-quality stream decryption (BF_CBC_STRIPE).
    // Without it, full streams will play corrupted. Use 16-char raw or 32-char hex.
    // DeezerAPI::setTrackXorKey("YOUR_16_BYTE_OR_32_HEX_KEY");

    // Apply dark mode stylesheet
    QString darkStyleSheet = R"(
        /* Main application dark theme */
        QMainWindow, QDialog, QWidget {
            background-color: #1e1e1e;
            color: #e0e0e0;
        }

        /* Menu bar */
        QMenuBar {
            background-color: #252525;
            color: #e0e0e0;
            border-bottom: 1px solid #3d3d3d;
        }

        QMenuBar::item {
            background-color: transparent;
            padding: 5px 10px;
        }

        QMenuBar::item:selected {
            background-color: #3d3d3d;
        }

        QMenuBar::item:pressed {
            background-color: #4a4a4a;
        }

        /* Menus */
        QMenu {
            background-color: #2b2b2b;
            color: #e0e0e0;
            border: 1px solid #3d3d3d;
        }

        QMenu::item {
            padding: 5px 30px 5px 20px;
        }

        QMenu::item:selected {
            background-color: #3d3d3d;
        }

        /* Status bar */
        QStatusBar {
            background-color: #252525;
            color: #e0e0e0;
            border-top: 1px solid #3d3d3d;
        }

        /* Tab widget */
        QTabWidget::pane {
            border: 1px solid #3d3d3d;
            background-color: #1e1e1e;
        }

        QTabBar::tab {
            background-color: #252525;
            color: #a0a0a0;
            border: 1px solid #3d3d3d;
            padding: 8px 20px;
            margin-right: 2px;
        }

        QTabBar::tab:selected {
            background-color: #3d3d3d;
            color: #e0e0e0;
            border-bottom: 2px solid #0e639c;
        }

        QTabBar::tab:hover {
            background-color: #2f2f2f;
        }

        /* Tables */
        QTableWidget, QTableView {
            background-color: #1e1e1e;
            alternate-background-color: #252525;
            color: #e0e0e0;
            gridline-color: #3d3d3d;
            border: 1px solid #3d3d3d;
            selection-background-color: #0e639c;
            selection-color: #ffffff;
        }

        QTableWidget::item, QTableView::item {
            padding: 5px;
        }

        QTableWidget::item:hover, QTableView::item:hover {
            background-color: #2f2f2f;
        }

        QHeaderView::section {
            background-color: #252525;
            color: #e0e0e0;
            padding: 5px;
            border: 1px solid #3d3d3d;
            font-weight: bold;
        }

        /* Scroll bars */
        QScrollBar:vertical {
            background-color: #1e1e1e;
            width: 14px;
            border: none;
        }

        QScrollBar::handle:vertical {
            background-color: #3d3d3d;
            min-height: 20px;
            border-radius: 7px;
            margin: 2px;
        }

        QScrollBar::handle:vertical:hover {
            background-color: #4a4a4a;
        }

        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }

        QScrollBar:horizontal {
            background-color: #1e1e1e;
            height: 14px;
            border: none;
        }

        QScrollBar::handle:horizontal {
            background-color: #3d3d3d;
            min-width: 20px;
            border-radius: 7px;
            margin: 2px;
        }

        QScrollBar::handle:horizontal:hover {
            background-color: #4a4a4a;
        }

        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }

        /* Buttons */
        QPushButton {
            background-color: #3d3d3d;
            color: #e0e0e0;
            border: 1px solid #555555;
            padding: 6px 16px;
            border-radius: 4px;
        }

        QPushButton:hover {
            background-color: #4a4a4a;
            border: 1px solid #666666;
        }

        QPushButton:pressed {
            background-color: #2f2f2f;
        }

        QPushButton:disabled {
            background-color: #2b2b2b;
            color: #666666;
        }

        /* Line edits */
        QLineEdit, QPlainTextEdit, QTextEdit {
            background-color: #2b2b2b;
            color: #e0e0e0;
            border: 1px solid #3d3d3d;
            padding: 5px;
            border-radius: 3px;
            selection-background-color: #0e639c;
        }

        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus {
            border: 1px solid #0e639c;
        }

        /* Labels */
        QLabel {
            color: #e0e0e0;
            background-color: transparent;
        }

        /* Sliders */
        QSlider::groove:horizontal {
            background-color: #3d3d3d;
            height: 6px;
            border-radius: 3px;
        }

        QSlider::handle:horizontal {
            background-color: #0e639c;
            width: 14px;
            margin: -4px 0;
            border-radius: 7px;
        }

        QSlider::handle:horizontal:hover {
            background-color: #1177bb;
        }

        QSlider::sub-page:horizontal {
            background-color: #0e639c;
            border-radius: 3px;
        }

        /* Checkboxes */
        QCheckBox {
            color: #e0e0e0;
            spacing: 5px;
        }

        QCheckBox::indicator {
            width: 18px;
            height: 18px;
            border: 1px solid #3d3d3d;
            border-radius: 3px;
            background-color: #2b2b2b;
        }

        QCheckBox::indicator:checked {
            background-color: #0e639c;
            border: 1px solid #0e639c;
        }

        QCheckBox::indicator:hover {
            border: 1px solid #4a4a4a;
        }

        /* Combo boxes */
        QComboBox {
            background-color: #2b2b2b;
            color: #e0e0e0;
            border: 1px solid #3d3d3d;
            padding: 5px;
            border-radius: 3px;
        }

        QComboBox:hover {
            border: 1px solid #4a4a4a;
        }

        QComboBox::drop-down {
            border: none;
            width: 20px;
        }

        QComboBox QAbstractItemView {
            background-color: #2b2b2b;
            color: #e0e0e0;
            selection-background-color: #0e639c;
            border: 1px solid #3d3d3d;
        }

        /* Tooltips */
        QToolTip {
            background-color: #2b2b2b;
            color: #e0e0e0;
            border: 1px solid #3d3d3d;
            padding: 5px;
        }

        /* Splitter handle */
        QSplitter::handle:horizontal {
            background-color: #3d3d3d;
            width: 1px;
        }

        /* Player controls bottom bar */
        PlayerControls {
            background-color: #1a1a1a;
            border-top: 1px solid #333;
        }
    )";

    app.setStyleSheet(darkStyleSheet);

    // Create and show main window
    MainWindow window;
    window.show();

    return app.exec();
}
