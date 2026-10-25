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

#include <cstring>

#include <windows.h>
#include <shellscalingapi.h>

#include <QApplication>
#include <QScreen>
#include <QScopeGuard>
#include <QStringList>
#include <QTest>
#include <QWindow>

#include "base/global.h"
#include "base/logger.h"
#include "gui/windowsdpirecovery.h"

namespace
{
    QStringList messages;
    UINT simulatedDpi = 96;

    HRESULT WINAPI simulatedMonitorDpi(HMONITOR, MONITOR_DPI_TYPE, UINT *x, UINT *y)
    {
        *x = simulatedDpi;
        *y = simulatedDpi;
        return S_OK;
    }

    // Override only the test process's Qt Windows plugin import. This simulates
    // temporary bad monitor information without changing Windows display settings.
    class MonitorDpiOverride final
    {
    public:
        MonitorDpiOverride()
        {
            auto *module = reinterpret_cast<BYTE *>(GetModuleHandleW(L"qwindows.dll"));
            if (!module)
                return;
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
            const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            auto *descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(module + directory.VirtualAddress);
            for (; descriptor->Name; ++descriptor)
            {
                if (!descriptor->OriginalFirstThunk)
                    continue;
                const auto *names = reinterpret_cast<const IMAGE_THUNK_DATA *>(module + descriptor->OriginalFirstThunk);
                auto *addresses = reinterpret_cast<IMAGE_THUNK_DATA *>(module + descriptor->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++addresses)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                        continue;
                    const auto *name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(module + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char *>(name->Name), "GetDpiForMonitor") != 0)
                        continue;
                    m_slot = &addresses->u1.Function;
                    m_original = *m_slot;
                    replace(reinterpret_cast<quintptr>(&simulatedMonitorDpi));
                    return;
                }
            }
        }

        ~MonitorDpiOverride()
        {
            if (m_slot)
                replace(m_original);
        }

        bool isActive() const { return m_slot && m_changed; }

    private:
        void replace(quintptr address)
        {
            DWORD protection = 0;
            if (!VirtualProtect(m_slot, sizeof(*m_slot), PAGE_READWRITE, &protection))
                return;
            *m_slot = address;
            DWORD unused = 0;
            VirtualProtect(m_slot, sizeof(*m_slot), protection, &unused);
            m_changed = true;
        }

        quintptr *m_slot = nullptr;
        quintptr m_original = 0;
        bool m_changed = false;
    };

    BOOL CALLBACK notifyQtDisplayObserver(HWND window, LPARAM)
    {
        wchar_t name[256] {};
        GetClassNameW(window, name, 256);
        if (QString::fromWCharArray(name).contains(u"ScreenChangeObserverWindow"))
            SendMessageW(window, WM_DISPLAYCHANGE, 32, 0);
        return TRUE;
    }

    void refreshScreens()
    {
        EnumThreadWindows(GetCurrentThreadId(), notifyQtDisplayObserver, 0);
        QCoreApplication::processEvents();
    }
}

// Capture the component's normal execution-log messages in this standalone test.
void LogMsg(const QString &message, const Log::MsgType &)
{
    messages.append(message);
}

class TestWindowsDpiRecovery final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (!GetModuleHandleW(L"qwindows.dll"))
            QSKIP("DPI fault injection requires the dynamic Qt Windows plugin.");
    }

    void healthyDisplayIsUnchanged()
    {
        messages.clear();
        const qreal originalScale = qApp->primaryScreen()->devicePixelRatio();
        QObject owner;
        initializeWindowsDpiRecovery(&owner);
        QTest::qWait(2200);
        QCOMPARE(qApp->primaryScreen()->devicePixelRatio(), originalScale);
        QVERIFY(messages.isEmpty());
    }

    void staleScreenScaleRecovers()
    {
        messages.clear();
        QScreen *screen = qApp->primaryScreen();
        const qreal originalScale = screen->devicePixelRatio();
        simulatedDpi = qFuzzyCompare(originalScale, qreal(1)) ? 192 : 96;
        {
            MonitorDpiOverride override;
            if (!override.isActive())
                QSKIP("Test requires the dynamic Qt Windows plugin with an imported GetDpiForMonitor.");
            refreshScreens();
            QCOMPARE(screen->devicePixelRatio(), simulatedDpi / qreal(96));
        }

        QObject owner;
        initializeWindowsDpiRecovery(&owner);
        QTRY_COMPARE_WITH_TIMEOUT(screen->devicePixelRatio(), originalScale, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(messages.contains(u"Display scaling recovery: Qt now matches Windows."_s), 3000);
        QVERIFY(messages.first().contains(u"refreshing Qt screens"));
    }

    void explicitScaleOverrideIsRespected()
    {
        messages.clear();
        QScreen *screen = qApp->primaryScreen();
        const qreal originalScale = screen->devicePixelRatio();
        simulatedDpi = qFuzzyCompare(originalScale, qreal(1)) ? 192 : 96;
        {
            MonitorDpiOverride override;
            QVERIFY(override.isActive());
            refreshScreens();
        }
        const qreal staleScale = screen->devicePixelRatio();
        qputenv("QT_SCALE_FACTOR", "2");
        const auto restore = qScopeGuard([]
        {
            qunsetenv("QT_SCALE_FACTOR");
            refreshScreens();
        });
        QObject owner;
        initializeWindowsDpiRecovery(&owner);
        QTest::qWait(2200);
        QCOMPARE(screen->devicePixelRatio(), staleScale);
        QVERIFY(messages.isEmpty());
    }

    void transientMismatchDoesNotTriggerRecovery()
    {
        messages.clear();
        QScreen *screen = qApp->primaryScreen();
        const qreal originalScale = screen->devicePixelRatio();
        simulatedDpi = qFuzzyCompare(originalScale, qreal(1)) ? 192 : 96;
        {
            MonitorDpiOverride override;
            QVERIFY(override.isActive());
            refreshScreens();
        }
        QObject owner;
        initializeWindowsDpiRecovery(&owner);
        QTest::qWait(1200);
        QVERIFY(messages.isEmpty());
        refreshScreens();
        QTest::qWait(1200);
        QCOMPARE(screen->devicePixelRatio(), originalScale);
        QVERIFY(messages.isEmpty());
    }

    void resumeRestartsChecksAfterTheyExpire()
    {
        messages.clear();
        QObject owner;
        initializeWindowsDpiRecovery(&owner);
        QTest::qWait(11000);

        QScreen *screen = qApp->primaryScreen();
        const qreal originalScale = screen->devicePixelRatio();
        simulatedDpi = qFuzzyCompare(originalScale, qreal(1)) ? 192 : 96;
        {
            MonitorDpiOverride override;
            QVERIFY(override.isActive());
            refreshScreens();
        }
        QWindow notificationWindow;
        notificationWindow.create();
        SendMessageW(reinterpret_cast<HWND>(notificationWindow.winId()), WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
        QTRY_COMPARE_WITH_TIMEOUT(screen->devicePixelRatio(), originalScale, 6000);
    }

    void persistentMismatchStopsRetrying()
    {
        messages.clear();
        QScreen *screen = qApp->primaryScreen();
        const qreal originalScale = screen->devicePixelRatio();
        simulatedDpi = qFuzzyCompare(originalScale, qreal(1)) ? 192 : 96;
        const auto restore = qScopeGuard([] { refreshScreens(); });
        MonitorDpiOverride override;
        QVERIFY(override.isActive());
        refreshScreens();
        QObject owner;
        initializeWindowsDpiRecovery(&owner);
        QTRY_VERIFY_WITH_TIMEOUT(!messages.isEmpty() && messages.last().contains(u"scaling still differs"), 14000);
        const qsizetype messageCount = messages.size();
        QTest::qWait(2200);
        QCOMPARE(messages.size(), messageCount);
    }
};

QTEST_MAIN(TestWindowsDpiRecovery)

#include "testwindowsdpirecovery.moc"
