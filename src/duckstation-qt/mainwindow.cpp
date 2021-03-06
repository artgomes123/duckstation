#include "mainwindow.h"
#include "aboutdialog.h"
#include "autoupdaterdialog.h"
#include "common/assert.h"
#include "core/game_list.h"
#include "core/host_display.h"
#include "core/settings.h"
#include "core/system.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "gamepropertiesdialog.h"
#include "qtdisplaywidget.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtGui/QCursor>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QActionGroup>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyleFactory>
#include <cmath>

static constexpr char DISC_IMAGE_FILTER[] = QT_TRANSLATE_NOOP(
  "MainWindow",
  "All File Types (*.bin *.img *.cue *.chd *.exe *.psexe *.psf);;Single-Track Raw Images (*.bin *.img);;Cue Sheets "
  "(*.cue);;MAME CHD Images (*.chd);;PlayStation Executables (*.exe *.psexe);;Portable Sound Format Files "
  "(*.psf);;Playlists (*.m3u)");

ALWAYS_INLINE static QString getWindowTitle()
{
  return QStringLiteral("DuckStation %1 (%2)").arg(g_scm_tag_str).arg(g_scm_branch_str);
}

MainWindow::MainWindow(QtHostInterface* host_interface)
  : QMainWindow(nullptr), m_unthemed_style_name(QApplication::style()->objectName()), m_host_interface(host_interface)
{
  m_host_interface->setMainWindow(this);

  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();
  updateTheme();

  resize(800, 700);
}

MainWindow::~MainWindow()
{
  Assert(!m_display_widget);
  m_host_interface->setMainWindow(nullptr);
}

void MainWindow::reportError(const QString& message)
{
  QMessageBox::critical(this, tr("DuckStation"), message, QMessageBox::Ok);
  focusDisplayWidget();
}

void MainWindow::reportMessage(const QString& message)
{
  m_ui.statusBar->showMessage(message, 2000);
}

bool MainWindow::confirmMessage(const QString& message)
{
  const int result = QMessageBox::question(this, tr("DuckStation"), message);
  focusDisplayWidget();

  return (result == QMessageBox::Yes);
}

QtDisplayWidget* MainWindow::createDisplay(QThread* worker_thread, const QString& adapter_name, bool use_debug_device,
                                           bool fullscreen, bool render_to_main)
{
  Assert(!m_host_display && !m_display_widget);
  Assert(!fullscreen || !render_to_main);

  m_display_widget = new QtDisplayWidget((!fullscreen && render_to_main) ? m_ui.mainContainer : nullptr);
  m_display_widget->setWindowTitle(windowTitle());
  m_display_widget->setWindowIcon(windowIcon());

  if (fullscreen)
  {
    m_display_widget->showFullScreen();
    m_display_widget->setCursor(Qt::BlankCursor);
  }
  else if (!render_to_main)
  {
    m_display_widget->showNormal();
  }
  else
  {
    m_ui.mainContainer->insertWidget(1, m_display_widget);
    switchToEmulationView();
  }

  // we need the surface visible.. this might be able to be replaced with something else
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    reportError(QStringLiteral("Failed to get window info from widget"));
    destroyDisplayWidget();
    return nullptr;
  }

  m_host_display = m_host_interface->createHostDisplay();
  if (!m_host_display || !m_host_display->CreateRenderDevice(wi.value(), adapter_name.toStdString(), use_debug_device))
  {
    reportError(tr("Failed to create host display device context."));
    destroyDisplayWidget();
    return nullptr;
  }

  m_host_display->DoneRenderContextCurrent();
  return m_display_widget;
}

QtDisplayWidget* MainWindow::updateDisplay(QThread* worker_thread, bool fullscreen, bool render_to_main)
{
  const bool is_fullscreen = m_display_widget->isFullScreen();
  const bool is_rendering_to_main = (!is_fullscreen && m_display_widget->parent());
  if (fullscreen == is_fullscreen && is_rendering_to_main == render_to_main)
    return m_display_widget;

  m_host_display->DestroyRenderSurface();

  destroyDisplayWidget();
  m_display_widget = new QtDisplayWidget((!fullscreen && render_to_main) ? m_ui.mainContainer : nullptr);
  m_display_widget->setWindowTitle(windowTitle());
  m_display_widget->setWindowIcon(windowIcon());

  if (fullscreen)
  {
    m_display_widget->showFullScreen();
    m_display_widget->setCursor(Qt::BlankCursor);
  }
  else if (!render_to_main)
  {
    m_display_widget->showNormal();
  }
  else
  {
    m_ui.mainContainer->insertWidget(1, m_display_widget);
    switchToEmulationView();
  }

  // we need the surface visible.. this might be able to be replaced with something else
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    reportError(QStringLiteral("Failed to get new window info from widget"));
    destroyDisplayWidget();
    return nullptr;
  }

  if (!m_host_display->ChangeRenderWindow(wi.value()))
    Panic("Failed to recreate surface on new widget.");

  m_display_widget->setFocus();

  QSignalBlocker blocker(m_ui.actionFullscreen);
  m_ui.actionFullscreen->setChecked(fullscreen);
  return m_display_widget;
}

void MainWindow::destroyDisplay()
{
  DebugAssert(m_host_display && m_display_widget);
  m_host_display = nullptr;
  destroyDisplayWidget();
}

void MainWindow::destroyDisplayWidget()
{
  if (!m_display_widget)
    return;

  if (m_display_widget->parent())
  {
    switchToGameListView();
    m_ui.mainContainer->removeWidget(m_display_widget);
  }

  delete m_display_widget;
  m_display_widget = nullptr;
}

void MainWindow::focusDisplayWidget()
{
  if (m_ui.mainContainer->currentIndex() != 1)
    return;

  m_display_widget->setFocus();
}

void MainWindow::onEmulationStarting()
{
  m_emulation_running = true;
  updateEmulationActions(true, false);

  // ensure it gets updated, since the boot can take a while
  QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::onEmulationStarted()
{
  updateEmulationActions(false, true);
}

void MainWindow::onEmulationStopped()
{
  m_emulation_running = false;
  updateEmulationActions(false, false);
  switchToGameListView();
}

void MainWindow::onEmulationPaused(bool paused)
{
  QSignalBlocker blocker(m_ui.actionPause);
  m_ui.actionPause->setChecked(paused);
}

void MainWindow::onStateSaved(const QString& game_code, bool global, qint32 slot)
{
  // don't bother updating for the resume state since we're powering off anyway
  if (slot < 0)
    return;

  m_host_interface->populateSaveStateMenus(game_code.toStdString().c_str(), m_ui.menuLoadState, m_ui.menuSaveState);
}

void MainWindow::onSystemPerformanceCountersUpdated(float speed, float fps, float vps, float average_frame_time,
                                                    float worst_frame_time)
{
  m_status_speed_widget->setText(QStringLiteral("%1%").arg(speed, 0, 'f', 0));
  m_status_fps_widget->setText(
    QStringLiteral("FPS: %1/%2").arg(std::round(fps), 0, 'f', 0).arg(std::round(vps), 0, 'f', 0));
  m_status_frame_time_widget->setText(
    QStringLiteral("%1ms average, %2ms worst").arg(average_frame_time, 0, 'f', 2).arg(worst_frame_time, 0, 'f', 2));
}

void MainWindow::onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title)
{
  m_host_interface->populateSaveStateMenus(game_code.toStdString().c_str(), m_ui.menuLoadState, m_ui.menuSaveState);
  if (game_title.isEmpty())
    setWindowTitle(getWindowTitle());
  else
    setWindowTitle(game_title);

  if (m_display_widget)
    m_display_widget->setWindowTitle(windowTitle());
}

void MainWindow::onStartDiscActionTriggered()
{
  QString filename =
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr);
  if (filename.isEmpty())
    return;

  SystemBootParameters boot_params;
  boot_params.filename = filename.toStdString();
  m_host_interface->bootSystem(boot_params);
}

void MainWindow::onStartBIOSActionTriggered()
{
  SystemBootParameters boot_params;
  m_host_interface->bootSystem(boot_params);
}

void MainWindow::onChangeDiscFromFileActionTriggered()
{
  QString filename =
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr);
  if (filename.isEmpty())
    return;

  m_host_interface->changeDisc(filename);
}

void MainWindow::onChangeDiscFromGameListActionTriggered()
{
  m_host_interface->pauseSystem(true);
  switchToGameListView();
}

void MainWindow::onChangeDiscFromPlaylistMenuAboutToShow()
{
  m_host_interface->populatePlaylistEntryMenu(m_ui.menuChangeDiscFromPlaylist);
}

void MainWindow::onChangeDiscFromPlaylistMenuAboutToHide()
{
  m_ui.menuChangeDiscFromPlaylist->clear();
}

void MainWindow::onRemoveDiscActionTriggered()
{
  m_host_interface->changeDisc(QString());
}

void MainWindow::onViewToolbarActionToggled(bool checked)
{
  m_host_interface->SetBoolSettingValue("UI", "ShowToolbar", checked);
  m_ui.toolBar->setVisible(checked);
}

void MainWindow::onViewStatusBarActionToggled(bool checked)
{
  m_host_interface->SetBoolSettingValue("UI", "ShowStatusBar", checked);
  m_ui.statusBar->setVisible(checked);
}

void MainWindow::onViewGameListActionTriggered()
{
  if (m_emulation_running)
    m_host_interface->pauseSystem(true);
  switchToGameListView();
}

void MainWindow::onViewSystemDisplayTriggered()
{
  if (m_emulation_running)
  {
    switchToEmulationView();
    m_host_interface->pauseSystem(false);
  }
}

void MainWindow::onGitHubRepositoryActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/stenzek/duckstation/");
}

void MainWindow::onIssueTrackerActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/stenzek/duckstation/issues");
}

void MainWindow::onDiscordServerActionTriggered()
{
  QtUtils::OpenURL(this, "https://discord.gg/Buktv3t");
}

void MainWindow::onAboutActionTriggered()
{
  AboutDialog about(this);
  about.exec();
}

void MainWindow::onGameListEntrySelected(const GameListEntry* entry)
{
  if (!entry)
  {
    m_ui.statusBar->clearMessage();
    m_host_interface->populateSaveStateMenus("", m_ui.menuLoadState, m_ui.menuSaveState);
    return;
  }

  m_ui.statusBar->showMessage(QString::fromStdString(entry->path));
  m_host_interface->populateSaveStateMenus(entry->code.c_str(), m_ui.menuLoadState, m_ui.menuSaveState);
}

void MainWindow::onGameListEntryDoubleClicked(const GameListEntry* entry)
{
  // if we're not running, boot the system, otherwise swap discs
  QString path = QString::fromStdString(entry->path);
  if (!m_emulation_running)
  {
    if (!entry->code.empty() && m_host_interface->GetBoolSettingValue("Main", "SaveStateOnExit", true))
    {
      m_host_interface->resumeSystemFromState(path, true);
    }
    else
    {
      SystemBootParameters boot_params;
      boot_params.filename = path.toStdString();
      m_host_interface->bootSystem(boot_params);
    }
  }
  else
  {
    m_host_interface->changeDisc(path);
    m_host_interface->pauseSystem(false);
    switchToEmulationView();
  }
}

void MainWindow::onGameListContextMenuRequested(const QPoint& point, const GameListEntry* entry)
{
  QMenu menu;

  // Hopefully this pointer doesn't disappear... it shouldn't.
  if (entry)
  {
    connect(menu.addAction(tr("Properties...")), &QAction::triggered,
            [this, entry]() { GamePropertiesDialog::showForEntry(m_host_interface, entry, this); });

    connect(menu.addAction(tr("Open Containing Directory...")), &QAction::triggered, [this, entry]() {
      const QFileInfo fi(QString::fromStdString(entry->path));
      QtUtils::OpenURL(this, QUrl::fromLocalFile(fi.absolutePath()));
    });

    menu.addSeparator();

    if (!m_emulation_running)
    {
      if (!entry->code.empty())
      {
        m_host_interface->populateGameListContextMenu(entry->code.c_str(), this, &menu);
        menu.addSeparator();
      }

      connect(menu.addAction(tr("Default Boot")), &QAction::triggered,
              [this, entry]() { m_host_interface->bootSystem(SystemBootParameters(entry->path)); });

      connect(menu.addAction(tr("Fast Boot")), &QAction::triggered, [this, entry]() {
        SystemBootParameters boot_params(entry->path);
        boot_params.override_fast_boot = true;
        m_host_interface->bootSystem(boot_params);
      });

      connect(menu.addAction(tr("Full Boot")), &QAction::triggered, [this, entry]() {
        SystemBootParameters boot_params(entry->path);
        boot_params.override_fast_boot = false;
        m_host_interface->bootSystem(boot_params);
      });
    }
    else
    {
      connect(menu.addAction(tr("Change Disc")), &QAction::triggered, [this, entry]() {
        m_host_interface->changeDisc(QString::fromStdString(entry->path));
        m_host_interface->pauseSystem(false);
        switchToEmulationView();
      });
    }

    menu.addSeparator();
  }

  connect(menu.addAction(tr("Add Search Directory...")), &QAction::triggered,
          [this]() { getSettingsDialog()->getGameListSettingsWidget()->addSearchDirectory(this); });

  menu.exec(point);
}

void MainWindow::setupAdditionalUi()
{
  setWindowTitle(getWindowTitle());

  const bool toolbar_visible = m_host_interface->GetBoolSettingValue("UI", "ShowToolbar", true);
  m_ui.actionViewToolbar->setChecked(toolbar_visible);
  m_ui.toolBar->setVisible(toolbar_visible);

  const bool status_bar_visible = m_host_interface->GetBoolSettingValue("UI", "ShowStatusBar", true);
  m_ui.actionViewStatusBar->setChecked(status_bar_visible);
  m_ui.statusBar->setVisible(status_bar_visible);

  m_game_list_widget = new GameListWidget(m_ui.mainContainer);
  m_game_list_widget->initialize(m_host_interface);
  m_ui.mainContainer->insertWidget(0, m_game_list_widget);
  m_ui.mainContainer->setCurrentIndex(0);

  m_status_speed_widget = new QLabel(m_ui.statusBar);
  m_status_speed_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_speed_widget->setFixedSize(40, 16);
  m_status_speed_widget->hide();

  m_status_fps_widget = new QLabel(m_ui.statusBar);
  m_status_fps_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_fps_widget->setFixedSize(80, 16);
  m_status_fps_widget->hide();

  m_status_frame_time_widget = new QLabel(m_ui.statusBar);
  m_status_frame_time_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_frame_time_widget->setFixedSize(190, 16);
  m_status_frame_time_widget->hide();

  updateDebugMenuVisibility();

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
  {
    const CPUExecutionMode mode = static_cast<CPUExecutionMode>(i);
    QAction* action = m_ui.menuCPUExecutionMode->addAction(tr(Settings::GetCPUExecutionModeDisplayName(mode)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, mode]() {
      m_host_interface->SetStringSettingValue("CPU", "ExecutionMode", Settings::GetCPUExecutionModeName(mode));
      m_host_interface->applySettings();
      updateDebugMenuCPUExecutionMode();
    });
  }
  updateDebugMenuCPUExecutionMode();

  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    const GPURenderer renderer = static_cast<GPURenderer>(i);
    QAction* action = m_ui.menuRenderer->addAction(tr(Settings::GetRendererDisplayName(renderer)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, renderer]() {
      m_host_interface->SetStringSettingValue("GPU", "Renderer", Settings::GetRendererName(renderer));
      m_host_interface->applySettings();
      updateDebugMenuGPURenderer();
    });
  }
  updateDebugMenuGPURenderer();

  const QString current_language(
    QString::fromStdString(m_host_interface->GetStringSettingValue("Main", "Language", "")));
  QActionGroup* language_group = new QActionGroup(m_ui.menuSettingsLanguage);
  for (const std::pair<QString, QString>& it : m_host_interface->getAvailableLanguageList())
  {
    QAction* action = language_group->addAction(it.first);
    action->setCheckable(true);
    action->setChecked(current_language == it.second);
    m_ui.menuSettingsLanguage->addAction(action);
    action->setData(it.second);
    connect(action, &QAction::triggered, [this, action]() {
      const QString new_language = action->data().toString();
      m_host_interface->SetStringSettingValue("Main", "Language", new_language.toUtf8().constData());
      QMessageBox::information(this, tr("DuckStation"),
                               tr("Language changed. Please restart the application to apply."));
    });
  }
}

void MainWindow::updateEmulationActions(bool starting, bool running)
{
  m_ui.actionStartDisc->setDisabled(starting || running);
  m_ui.actionStartBios->setDisabled(starting || running);
  m_ui.actionResumeLastState->setDisabled(starting || running);

  m_ui.actionPowerOff->setDisabled(starting || !running);
  m_ui.actionReset->setDisabled(starting || !running);
  m_ui.actionPause->setDisabled(starting || !running);
  m_ui.actionChangeDisc->setDisabled(starting || !running);
  m_ui.actionScreenshot->setDisabled(starting || !running);
  m_ui.actionViewSystemDisplay->setEnabled(starting || running);
  m_ui.menuChangeDisc->setDisabled(starting || !running);

  m_ui.actionSaveState->setDisabled(starting || !running);
  m_ui.menuSaveState->setDisabled(starting || !running);

  m_ui.actionFullscreen->setDisabled(starting || !running);

  if (running && m_status_speed_widget->isHidden())
  {
    m_status_speed_widget->show();
    m_status_fps_widget->show();
    m_status_frame_time_widget->show();
    m_ui.statusBar->addPermanentWidget(m_status_speed_widget);
    m_ui.statusBar->addPermanentWidget(m_status_fps_widget);
    m_ui.statusBar->addPermanentWidget(m_status_frame_time_widget);
  }
  else if (!running && m_status_speed_widget->isVisible())
  {
    m_ui.statusBar->removeWidget(m_status_speed_widget);
    m_ui.statusBar->removeWidget(m_status_fps_widget);
    m_ui.statusBar->removeWidget(m_status_frame_time_widget);
    m_status_speed_widget->hide();
    m_status_fps_widget->hide();
    m_status_frame_time_widget->hide();
  }

  if (starting || running)
  {
    if (!m_ui.toolBar->actions().contains(m_ui.actionPowerOff))
    {
      m_ui.toolBar->insertAction(m_ui.actionResumeLastState, m_ui.actionPowerOff);
      m_ui.toolBar->removeAction(m_ui.actionResumeLastState);
    }
  }
  else
  {
    if (!m_ui.toolBar->actions().contains(m_ui.actionResumeLastState))
    {
      m_ui.toolBar->insertAction(m_ui.actionPowerOff, m_ui.actionResumeLastState);
      m_ui.toolBar->removeAction(m_ui.actionPowerOff);
    }
  }

  m_ui.statusBar->clearMessage();
}

void MainWindow::switchToGameListView()
{
  m_ui.mainContainer->setCurrentIndex(0);
}

void MainWindow::switchToEmulationView()
{
  if (m_display_widget->parent())
    m_ui.mainContainer->setCurrentIndex(1);
  m_display_widget->setFocus();
}

void MainWindow::connectSignals()
{
  updateEmulationActions(false, false);
  onEmulationPaused(false);

  connect(m_ui.actionStartDisc, &QAction::triggered, this, &MainWindow::onStartDiscActionTriggered);
  connect(m_ui.actionStartBios, &QAction::triggered, this, &MainWindow::onStartBIOSActionTriggered);
  connect(m_ui.actionResumeLastState, &QAction::triggered, m_host_interface,
          &QtHostInterface::resumeSystemFromMostRecentState);
  connect(m_ui.actionChangeDisc, &QAction::triggered, [this] { m_ui.menuChangeDisc->exec(QCursor::pos()); });
  connect(m_ui.actionChangeDiscFromFile, &QAction::triggered, this, &MainWindow::onChangeDiscFromFileActionTriggered);
  connect(m_ui.actionChangeDiscFromGameList, &QAction::triggered, this,
          &MainWindow::onChangeDiscFromGameListActionTriggered);
  connect(m_ui.menuChangeDiscFromPlaylist, &QMenu::aboutToShow, this,
          &MainWindow::onChangeDiscFromPlaylistMenuAboutToShow);
  connect(m_ui.menuChangeDiscFromPlaylist, &QMenu::aboutToHide, this,
          &MainWindow::onChangeDiscFromPlaylistMenuAboutToHide);
  connect(m_ui.actionRemoveDisc, &QAction::triggered, this, &MainWindow::onRemoveDiscActionTriggered);
  connect(m_ui.actionAddGameDirectory, &QAction::triggered,
          [this]() { getSettingsDialog()->getGameListSettingsWidget()->addSearchDirectory(this); });
  connect(m_ui.actionPowerOff, &QAction::triggered, m_host_interface, &QtHostInterface::powerOffSystem);
  connect(m_ui.actionReset, &QAction::triggered, m_host_interface, &QtHostInterface::resetSystem);
  connect(m_ui.actionPause, &QAction::toggled, m_host_interface, &QtHostInterface::pauseSystem);
  connect(m_ui.actionScreenshot, &QAction::triggered, m_host_interface, &QtHostInterface::saveScreenshot);
  connect(m_ui.actionScanForNewGames, &QAction::triggered, this,
          [this]() { m_host_interface->refreshGameList(false, false); });
  connect(m_ui.actionRescanAllGames, &QAction::triggered, this,
          [this]() { m_host_interface->refreshGameList(true, false); });
  connect(m_ui.actionLoadState, &QAction::triggered, this, [this]() { m_ui.menuLoadState->exec(QCursor::pos()); });
  connect(m_ui.actionSaveState, &QAction::triggered, this, [this]() { m_ui.menuSaveState->exec(QCursor::pos()); });
  connect(m_ui.actionExit, &QAction::triggered, this, &MainWindow::close);
  connect(m_ui.actionFullscreen, &QAction::triggered, m_host_interface, &QtHostInterface::toggleFullscreen);
  connect(m_ui.actionSettings, &QAction::triggered, [this]() { doSettings(SettingsDialog::Category::Count); });
  connect(m_ui.actionGeneralSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::GeneralSettings); });
  connect(m_ui.actionConsoleSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::ConsoleSettings); });
  connect(m_ui.actionGameListSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::GameListSettings); });
  connect(m_ui.actionHotkeySettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::HotkeySettings); });
  connect(m_ui.actionControllerSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::ControllerSettings); });
  connect(m_ui.actionMemoryCardSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::MemoryCardSettings); });
  connect(m_ui.actionGPUSettings, &QAction::triggered, [this]() { doSettings(SettingsDialog::Category::GPUSettings); });
  connect(m_ui.actionAudioSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::AudioSettings); });
  connect(m_ui.actionAdvancedSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::AdvancedSettings); });
  connect(m_ui.actionViewToolbar, &QAction::toggled, this, &MainWindow::onViewToolbarActionToggled);
  connect(m_ui.actionViewStatusBar, &QAction::toggled, this, &MainWindow::onViewStatusBarActionToggled);
  connect(m_ui.actionViewGameList, &QAction::triggered, this, &MainWindow::onViewGameListActionTriggered);
  connect(m_ui.actionViewSystemDisplay, &QAction::triggered, this, &MainWindow::onViewSystemDisplayTriggered);
  connect(m_ui.actionGitHubRepository, &QAction::triggered, this, &MainWindow::onGitHubRepositoryActionTriggered);
  connect(m_ui.actionIssueTracker, &QAction::triggered, this, &MainWindow::onIssueTrackerActionTriggered);
  connect(m_ui.actionDiscordServer, &QAction::triggered, this, &MainWindow::onDiscordServerActionTriggered);
  connect(m_ui.actionAbout, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);
  connect(m_ui.actionCheckForUpdates, &QAction::triggered, [this]() { checkForUpdates(true); });

  connect(m_host_interface, &QtHostInterface::errorReported, this, &MainWindow::reportError,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::messageReported, this, &MainWindow::reportMessage);
  connect(m_host_interface, &QtHostInterface::messageConfirmed, this, &MainWindow::confirmMessage,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::createDisplayRequested, this, &MainWindow::createDisplay,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::destroyDisplayRequested, this, &MainWindow::destroyDisplay);
  connect(m_host_interface, &QtHostInterface::updateDisplayRequested, this, &MainWindow::updateDisplay,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::focusDisplayWidgetRequested, this, &MainWindow::focusDisplayWidget);
  connect(m_host_interface, &QtHostInterface::emulationStarting, this, &MainWindow::onEmulationStarting);
  connect(m_host_interface, &QtHostInterface::emulationStarted, this, &MainWindow::onEmulationStarted);
  connect(m_host_interface, &QtHostInterface::emulationStopped, this, &MainWindow::onEmulationStopped);
  connect(m_host_interface, &QtHostInterface::emulationPaused, this, &MainWindow::onEmulationPaused);
  connect(m_host_interface, &QtHostInterface::stateSaved, this, &MainWindow::onStateSaved);
  connect(m_host_interface, &QtHostInterface::systemPerformanceCountersUpdated, this,
          &MainWindow::onSystemPerformanceCountersUpdated);
  connect(m_host_interface, &QtHostInterface::runningGameChanged, this, &MainWindow::onRunningGameChanged);
  connect(m_host_interface, &QtHostInterface::exitRequested, this, &MainWindow::close);

  // These need to be queued connections to stop crashing due to menus opening/closing and switching focus.
  connect(m_game_list_widget, &GameListWidget::entrySelected, this, &MainWindow::onGameListEntrySelected,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryDoubleClicked, this, &MainWindow::onGameListEntryDoubleClicked,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryContextMenuRequested, this,
          &MainWindow::onGameListContextMenuRequested);

  m_host_interface->populateSaveStateMenus(nullptr, m_ui.menuLoadState, m_ui.menuSaveState);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugDumpCPUtoVRAMCopies, "Debug",
                                               "DumpCPUToVRAMCopies");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugDumpVRAMtoCPUCopies, "Debug",
                                               "DumpVRAMToCPUCopies");
  connect(m_ui.actionDumpAudio, &QAction::toggled, [this](bool checked) {
    if (checked)
      m_host_interface->startDumpingAudio();
    else
      m_host_interface->stopDumpingAudio();
  });
  connect(m_ui.actionDumpRAM, &QAction::triggered, [this]() {
    const QString filename = QFileDialog::getSaveFileName(this, tr("Destination File"));
    if (filename.isEmpty())
      return;

    m_host_interface->dumpRAM(filename);
  });
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowVRAM, "Debug", "ShowVRAM");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowGPUState, "Debug", "ShowGPUState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowCDROMState, "Debug",
                                               "ShowCDROMState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowSPUState, "Debug", "ShowSPUState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowTimersState, "Debug",
                                               "ShowTimersState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowMDECState, "Debug",
                                               "ShowMDECState");

  addThemeToMenu(tr("Default"), QStringLiteral("default"));
  addThemeToMenu(tr("DarkFusion"), QStringLiteral("darkfusion"));
  addThemeToMenu(tr("QDarkStyle"), QStringLiteral("qdarkstyle"));
}

void MainWindow::addThemeToMenu(const QString& name, const QString& key)
{
  QAction* action = m_ui.menuSettingsTheme->addAction(name);
  action->setCheckable(true);
  action->setData(key);
  connect(action, &QAction::toggled, [this, key](bool) { setTheme(key); });
}

void MainWindow::setTheme(const QString& theme)
{
  m_host_interface->SetStringSettingValue("UI", "Theme", theme.toUtf8().constData());
  updateTheme();
}

void MainWindow::updateTheme()
{
  QString theme = QString::fromStdString(m_host_interface->GetStringSettingValue("UI", "Theme", "default"));
  if (theme == QStringLiteral("qdarkstyle"))
  {
    qApp->setStyle(m_unthemed_style_name);
    qApp->setPalette(QApplication::style()->standardPalette());

    QFile f(QStringLiteral(":qdarkstyle/style.qss"));
    if (f.open(QFile::ReadOnly | QFile::Text))
      qApp->setStyleSheet(f.readAll());
  }
  else if (theme == QStringLiteral("darkfusion"))
  {
    // adapted from https://gist.github.com/QuantumCD/6245215
    qApp->setStyle(QStyleFactory::create("Fusion"));

    const QColor lighterGray(75, 75, 75);
    const QColor darkGray(53, 53, 53);
    const QColor gray(128, 128, 128);
    const QColor black(25, 25, 25);
    const QColor blue(198, 238, 255);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, black);
    darkPalette.setColor(QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, blue);
    darkPalette.setColor(QPalette::Highlight, lighterGray);
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);

    darkPalette.setColor(QPalette::Active, QPalette::Button, gray.darker());
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);

    qApp->setPalette(darkPalette);

    qApp->setStyleSheet("QToolTip { color: #ffffff; background-color: #2a82da; border: 1px solid white; }");
  }
  else
  {
    qApp->setPalette(QApplication::style()->standardPalette());
    qApp->setStyleSheet(QString());
    qApp->setStyle(m_unthemed_style_name);
  }

  for (QObject* obj : m_ui.menuSettingsTheme->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
    {
      QVariant action_data(action->data());
      if (action_data.isValid())
      {
        QSignalBlocker blocker(action);
        action->setChecked(action_data == theme);
      }
    }
  }
}

SettingsDialog* MainWindow::getSettingsDialog()
{
  if (!m_settings_dialog)
    m_settings_dialog = new SettingsDialog(m_host_interface, this);

  return m_settings_dialog;
}

void MainWindow::doSettings(SettingsDialog::Category category)
{
  SettingsDialog* dlg = getSettingsDialog();
  if (!dlg->isVisible())
  {
    dlg->setModal(false);
    dlg->show();
  }

  if (category != SettingsDialog::Category::Count)
    dlg->setCategory(category);
}

void MainWindow::updateDebugMenuCPUExecutionMode()
{
  std::optional<CPUExecutionMode> current_mode =
    Settings::ParseCPUExecutionMode(m_host_interface->GetStringSettingValue("CPU", "ExecutionMode").c_str());
  if (!current_mode.has_value())
    return;

  const QString current_mode_display_name(tr(Settings::GetCPUExecutionModeDisplayName(current_mode.value())));
  for (QObject* obj : m_ui.menuCPUExecutionMode->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_mode_display_name);
  }
}

void MainWindow::updateDebugMenuGPURenderer()
{
  // update the menu with the new selected renderer
  std::optional<GPURenderer> current_renderer =
    Settings::ParseRendererName(m_host_interface->GetStringSettingValue("GPU", "Renderer").c_str());
  if (!current_renderer.has_value())
    return;

  const QString current_renderer_display_name(tr(Settings::GetRendererDisplayName(current_renderer.value())));
  for (QObject* obj : m_ui.menuRenderer->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_renderer_display_name);
  }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  m_host_interface->synchronousPowerOffSystem();
  QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent* event)
{
  if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
  {
    // TODO: This should check the render-to-main option.
    if (m_display_widget)
      m_host_interface->redrawDisplayWindow();
  }

  QMainWindow::changeEvent(event);
}

void MainWindow::startupUpdateCheck()
{
  if (!m_host_interface->GetBoolSettingValue("AutoUpdater", "CheckAtStartup", true))
    return;

  checkForUpdates(false);
}

void MainWindow::updateDebugMenuVisibility()
{
  const bool visible = m_host_interface->GetBoolSettingValue("Main", "ShowDebugMenu", false);
  m_ui.menuDebug->menuAction()->setVisible(visible);
}

void MainWindow::checkForUpdates(bool display_message)
{
  if (!AutoUpdaterDialog::isSupported())
  {
    if (display_message)
    {
      QMessageBox mbox(this);
      mbox.setWindowTitle(tr("Updater Error"));
      mbox.setTextFormat(Qt::RichText);

      QString message;
#ifdef WIN32
      message =
        tr("<p>Sorry, you are trying to update a DuckStation version which is not an official GitHub release. To "
           "prevent incompatibilities, the auto-updater is only enabled on official builds.</p>"
           "<p>To obtain an official build, please follow the instructions under \"Downloading and Running\" at the "
           "link below:</p>"
           "<p><a href=\"https://github.com/stenzek/duckstation/\">https://github.com/stenzek/duckstation/</a></p>");
#else
      message = tr("Automatic updating is not supported on the current platform.");
#endif

      mbox.setText(message);
      mbox.setIcon(QMessageBox::Critical);
      mbox.exec();
    }

    return;
  }

  if (m_auto_updater_dialog)
    return;

  m_auto_updater_dialog = new AutoUpdaterDialog(m_host_interface, this);
  connect(m_auto_updater_dialog, &AutoUpdaterDialog::updateCheckCompleted, this, &MainWindow::onUpdateCheckComplete);
  m_auto_updater_dialog->queueUpdateCheck(display_message);
}

void MainWindow::onUpdateCheckComplete()
{
  if (!m_auto_updater_dialog)
    return;

  m_auto_updater_dialog->deleteLater();
  m_auto_updater_dialog = nullptr;
}
