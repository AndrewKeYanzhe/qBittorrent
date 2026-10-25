/*
 * Bittorrent Client using Qt and libtorrent.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "windowsdpirecovery.h"

#include <windows.h>
#include <shellscalingapi.h>

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QtGui/qscreen_platform.h>
#endif

#include "base/global.h"
#include "base/logger.h"

namespace
{
    constexpr int CHECK_INTERVAL = 1000;
    constexpr int MAX_CHECKS = 10;

    struct MonitorScale
    {
        HMONITOR monitor = nullptr;
        QPoint position;
        qreal factor = 1;
    };

    BOOL CALLBACK collectMonitorScale(HMONITOR monitor, HDC, LPRECT, LPARAM data)
    {
        MONITORINFOEXW info {};
        info.cbSize = sizeof(info);
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (GetMonitorInfoW(monitor, &info) && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        {
            auto *scales = reinterpret_cast<QList<MonitorScale> *>(data);
            if (dpiX > 0)
                scales->append({monitor, {info.rcMonitor.left, info.rcMonitor.top}, dpiX / qreal(96)});
        }
        return TRUE;
    }

    struct DisplayRefresh
    {
        DEVMODEW mode {};
        bool delivered = false;
    };

    BOOL CALLBACK refreshQtScreens(HWND window, LPARAM data)
    {
        wchar_t className[256] {};
        if (!GetClassNameW(window, className, 256))
            return TRUE;

        const QString name = QString::fromWCharArray(className);
        if (!name.startsWith(u"Qt") || !name.contains(u"ScreenChangeObserverWindow"))
            return TRUE;

        auto *refresh = reinterpret_cast<DisplayRefresh *>(data);
        // Qt 6.6 and later use this hidden window to re-enumerate monitors and
        // propagate changed DPI to existing windows. Notify only our GUI thread's
        // observer; broadcasting would affect other applications. Do this outside
        // the native event filter, after Windows has restored the display.
        SendMessageW(window, WM_DISPLAYCHANGE, refresh->mode.dmBitsPerPel,
            MAKELPARAM(refresh->mode.dmPelsWidth, refresh->mode.dmPelsHeight));
        refresh->delivered = true;
        return FALSE;
    }

    class WindowsDpiRecovery final : public QObject, public QAbstractNativeEventFilter
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(WindowsDpiRecovery)

    public:
        explicit WindowsDpiRecovery(QObject *parent)
            : QObject(parent)
        {
            m_timer.setInterval(CHECK_INTERVAL);
            connect(&m_timer, &QTimer::timeout, this, &WindowsDpiRecovery::checkScaling);
            connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state)
            {
                if (state == Qt::ApplicationActive)
                    scheduleChecks();
            });

            // A never-shown native window receives broadcasts even in tray-only
            // operation. Display power notifications also cover monitor sleep
            // without a system suspend or a monitor topology change.
            m_notificationWindow.create();
            m_displayNotification = RegisterPowerSettingNotification(
                reinterpret_cast<HWND>(m_notificationWindow.winId()), &GUID_CONSOLE_DISPLAY_STATE,
                DEVICE_NOTIFY_WINDOW_HANDLE);
            qGuiApp->installNativeEventFilter(this);
            scheduleChecks();
        }

        ~WindowsDpiRecovery() override
        {
            if (QCoreApplication::instance())
                QCoreApplication::instance()->removeNativeEventFilter(this);
            if (m_displayNotification)
                UnregisterPowerSettingNotification(m_displayNotification);
        }

        bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override
        {
            if (eventType != "windows_generic_MSG")
                return false;

            const auto *msg = static_cast<const MSG *>(message);
            if ((msg->message == WM_DISPLAYCHANGE) || (msg->message == WM_DPICHANGED)
                || (msg->message == WM_SETTINGCHANGE))
            {
                scheduleChecks();
            }
            else if (msg->message == WM_POWERBROADCAST)
            {
                if ((msg->wParam == PBT_APMRESUMEAUTOMATIC) || (msg->wParam == PBT_APMRESUMESUSPEND))
                {
                    scheduleChecks();
                }
                else if ((msg->wParam == PBT_POWERSETTINGCHANGE) && msg->lParam)
                {
                    const auto *setting = reinterpret_cast<const POWERBROADCAST_SETTING *>(msg->lParam);
                    if (IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE)
                        && (setting->DataLength == sizeof(DWORD)) && (setting->Data[0] != 0))
                    {
                        scheduleChecks();
                    }
                }
            }
            return false;
        }

    private:
        void scheduleChecks()
        {
            if (m_refreshing)
                return;

            m_checksRemaining = MAX_CHECKS;
            // Do not restart an active timer: a burst of native messages must not
            // postpone recovery indefinitely.
            if (!m_timer.isActive())
                m_timer.start();
        }

        void checkScaling()
        {
            if (--m_checksRemaining == 0)
                m_timer.stop();

            // Respect explicit Qt scaling overrides and non-per-monitor-aware
            // configurations. Their scale need not equal Windows' monitor DPI.
            if (!qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")
                || !qEnvironmentVariableIsEmpty("QT_SCREEN_SCALE_FACTORS")
                || !qEnvironmentVariableIsEmpty("QT_FONT_DPI")
                || !qEnvironmentVariableIsEmpty("QT_ENABLE_HIGHDPI_SCALING")
                || (QGuiApplication::highDpiScaleFactorRoundingPolicy() != Qt::HighDpiScaleFactorRoundingPolicy::PassThrough)
                || (GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) != DPI_AWARENESS_PER_MONITOR_AWARE))
            {
                m_timer.stop();
                return;
            }

            QList<MonitorScale> scales;
            if (!EnumDisplayMonitors(nullptr, nullptr, collectMonitorScale, reinterpret_cast<LPARAM>(&scales)) || scales.isEmpty())
                return;

            QString mismatch;
            for (const QScreen *screen : QGuiApplication::screens())
            {
                HMONITOR nativeMonitor = nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
                if (const auto *nativeScreen = screen->nativeInterface<QNativeInterface::QWindowsScreen>())
                    nativeMonitor = nativeScreen->handle();
#endif
                bool found = false;
                for (const MonitorScale &scale : asConst(scales))
                {
                    // Recent Qt versions use a friendly display name, which does
                    // not equal MONITORINFOEX::szDevice. Prefer the native handle.
                    // On older Qt, Windows screen origins are native coordinates
                    // even when Qt scales the screen's width and height.
                    if (nativeMonitor ? (nativeMonitor != scale.monitor)
                                      : (screen->geometry().topLeft() != scale.position))
                        continue;

                    found = true;
                    if (!qFuzzyCompare(screen->devicePixelRatio(), scale.factor))
                    {
                        mismatch = u"%1: Qt scale %2, Windows scale %3"_s.arg(screen->name())
                            .arg(screen->devicePixelRatio()).arg(scale.factor);
                    }
                    break;
                }
                if (!found)
                    mismatch = u"%1: Qt screen no longer matches an attached Windows monitor"_s.arg(screen->name());
                if (!mismatch.isEmpty())
                    break;
            }

            if (mismatch.isEmpty())
            {
                if (m_recoveryPending)
                    LogMsg(tr("Display scaling recovery: Qt now matches Windows."), Log::INFO);
                m_recoveryPending = false;
                m_previousMismatch.clear();
                return;
            }

            // Require the same mismatch on two successive checks, avoiding
            // corrections while monitors are still reconnecting.
            if ((mismatch == m_previousMismatch) && (m_checksRemaining > 0))
            {
                DisplayRefresh refresh;
                refresh.mode.dmSize = sizeof(refresh.mode);
                if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &refresh.mode))
                {
                    m_refreshing = true;
                    EnumThreadWindows(GetCurrentThreadId(), refreshQtScreens, reinterpret_cast<LPARAM>(&refresh));
                    m_refreshing = false;
                    if (refresh.delivered && !m_recoveryPending)
                        LogMsg(tr("Display scaling recovery: refreshing Qt screens (%1).").arg(mismatch), Log::INFO);
                    m_recoveryPending = m_recoveryPending || refresh.delivered;
                }
            }
            m_previousMismatch = mismatch;

            if (m_checksRemaining == 0)
            {
                LogMsg(tr("Display scaling recovery: scaling still differs after retrying (%1).").arg(mismatch), Log::WARNING);
                m_recoveryPending = false;
                m_previousMismatch.clear();
            }
        }

        QWindow m_notificationWindow;
        QTimer m_timer;
        HPOWERNOTIFY m_displayNotification = nullptr;
        int m_checksRemaining = 0;
        bool m_refreshing = false;
        bool m_recoveryPending = false;
        QString m_previousMismatch;
    };
}

void initializeWindowsDpiRecovery(QObject *parent)
{
    new WindowsDpiRecovery(parent);
}

#include "windowsdpirecovery.moc"
