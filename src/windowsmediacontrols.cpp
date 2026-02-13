#include "windowsmediacontrols.h"
#include <windows.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.h>
#include <wrl/event.h>
#include <roapi.h>
#include <systemmediatransportcontrolsinterop.h>
#include <QDebug>
#include <QWidget>
#include <QWindow>
#include <QGuiApplication>

#include <windows.storage.streams.h>
#include <windows.foundation.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Media;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage::Streams;

class WindowsMediaControls::Private
{
public:
    ComPtr<ISystemMediaTransportControls> smtc;
    ComPtr<ISystemMediaTransportControlsDisplayUpdater> updater;
    ComPtr<IRandomAccessStreamReferenceStatics> streamRefStatics;
    EventRegistrationToken buttonPressedToken;
    WindowsMediaControls* q;

    Private(WindowsMediaControls* parent) : q(parent) {}

    bool initialize() {
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            qDebug() << "[SMTC] RoInitialize failed:" << Qt::hex << hr;
            return false;
        }

        ComPtr<ISystemMediaTransportControlsInterop> interop;
        hr = RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(),
            IID_PPV_ARGS(&interop));
        
        if (FAILED(hr)) {
            qDebug() << "[SMTC] GetActivationFactory failed:" << Qt::hex << hr;
            return false;
        }

        // Get the handle of the nearest widget or top-level window
        HWND hwnd = nullptr;
        QObject* current = q;
        while (current) {
            if (auto w = qobject_cast<QWidget*>(current)) {
                hwnd = (HWND)w->winId();
                if (hwnd) break;
            }
            current = current->parent();
        }

        if (!hwnd && !QGuiApplication::topLevelWindows().isEmpty()) {
            hwnd = (HWND)QGuiApplication::topLevelWindows().first()->winId();
        }

        if (!hwnd) {
            qDebug() << "[SMTC] No HWND found, will retry initialization later if needed";
            return false;
        }

        qDebug() << "[SMTC] Initializing for HWND:" << hwnd;

        hr = interop->GetForWindow(hwnd, IID_PPV_ARGS(&smtc));
        if (FAILED(hr)) {
            qDebug() << "[SMTC] GetForWindow failed:" << Qt::hex << hr;
            return false;
        }

        smtc->put_IsEnabled(true);
        smtc->put_IsPlayEnabled(true);
        smtc->put_IsPauseEnabled(true);
        smtc->put_IsNextEnabled(true);
        smtc->put_IsPreviousEnabled(true);
        smtc->put_PlaybackStatus(MediaPlaybackStatus_Closed); // Initial state

        auto handler = Callback<ITypedEventHandler<SystemMediaTransportControls*, SystemMediaTransportControlsButtonPressedEventArgs*>>(
            [this](ISystemMediaTransportControls*, ISystemMediaTransportControlsButtonPressedEventArgs* args) {
                SystemMediaTransportControlsButton button;
                if (SUCCEEDED(args->get_Button(&button))) {
                    switch (button) {
                        case SystemMediaTransportControlsButton_Play: emit q->playRequested(); break;
                        case SystemMediaTransportControlsButton_Pause: emit q->pauseRequested(); break;
                        case SystemMediaTransportControlsButton_Next: emit q->nextRequested(); break;
                        case SystemMediaTransportControlsButton_Previous: emit q->previousRequested(); break;
                        default: break;
                    }
                }
                return S_OK;
            });

        hr = smtc->add_ButtonPressed(handler.Get(), &buttonPressedToken);
        if (FAILED(hr)) qDebug() << "[SMTC] add_ButtonPressed failed:" << Qt::hex << hr;

        hr = smtc->get_DisplayUpdater(&updater);
        if (FAILED(hr)) qDebug() << "[SMTC] get_DisplayUpdater failed:" << Qt::hex << hr;

        // Initialize factory for thumbnails
        hr = RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(),
            IID_PPV_ARGS(&streamRefStatics));
        if (FAILED(hr)) qDebug() << "[SMTC] Failed to get RandomAccessStreamReference factory:" << Qt::hex << hr;

        qDebug() << "[SMTC] Initialized successfully";
        return SUCCEEDED(hr);
    }

    QWidget* parentWidget() {
        return qobject_cast<QWidget*>(q->parent());
    }
};

WindowsMediaControls::WindowsMediaControls(QObject *parent)
    : QObject(parent)
    , d(new Private(this))
{
    if (!d->initialize()) {
        qDebug() << "[SMTC] Failed to initialize Windows Media Controls";
    }
}

WindowsMediaControls::~WindowsMediaControls()
{
    if (d->smtc) {
        d->smtc->remove_ButtonPressed(d->buttonPressedToken);
    }
    delete d;
}

void WindowsMediaControls::setEnabled(bool enabled)
{
    if (d->smtc) d->smtc->put_IsEnabled(enabled);
}

void WindowsMediaControls::updateMetadata(const QString& title, const QString& artist, const QString& album, const QUrl& artUrl)
{
    if (!d->smtc && !d->initialize()) return;
    if (!d->updater) return;

    qDebug() << "[SMTC] Updating metadata:" << title << "by" << artist << "Art:" << artUrl.toString();

    d->updater->put_Type(MediaPlaybackType_Music);
    
    ComPtr<IMusicDisplayProperties> musicProps;
    if (SUCCEEDED(d->updater->get_MusicProperties(&musicProps))) {
        HString hTitle, hArtist;
        hTitle.Set((const wchar_t*)title.utf16());
        hArtist.Set((const wchar_t*)artist.utf16());
        
        musicProps->put_Title(hTitle.Get());
        musicProps->put_Artist(hArtist.Get());
    }

    if (d->streamRefStatics && artUrl.isValid()) {
        ComPtr<IUriRuntimeClassFactory> uriFactory;
        if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_Uri).Get(), IID_PPV_ARGS(&uriFactory)))) {
            HString hUri;
            QString urlString = artUrl.toString();  // Store temporary to prevent use-after-free
            hUri.Set((const wchar_t*)urlString.utf16());
            ComPtr<IUriRuntimeClass> uri;
            if (SUCCEEDED(uriFactory->CreateUri(hUri.Get(), &uri))) {
                ComPtr<IRandomAccessStreamReference> thumbnail;
                if (SUCCEEDED(d->streamRefStatics->CreateFromUri(uri.Get(), &thumbnail))) {
                    d->updater->put_Thumbnail(thumbnail.Get());
                }
            }
        }
    }

    d->updater->Update();
}

void WindowsMediaControls::updatePlaybackState(bool playing)
{
    if (!d->smtc && !d->initialize()) return;
    if (d->smtc) {
        qDebug() << "[SMTC] Updating playback state:" << (playing ? "Playing" : "Paused");
        d->smtc->put_PlaybackStatus(playing ? MediaPlaybackStatus_Playing : MediaPlaybackStatus_Paused);
    }
}
