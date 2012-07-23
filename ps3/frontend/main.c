/* RetroArch - A frontend for libretro.
 * Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 * Copyright (C) 2011-2012 - Daniel De Matteis
 *
 * RetroArch is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Found-
 * ation, either version 3 of the License, or (at your option) any later version.
 *
 * RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with RetroArch.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include "../../boolean.h"
#include <stddef.h>
#include <string.h>

#include <sdk_version.h>
#include <sys/process.h>
#ifdef HAVE_SYSUTILS
#include <sysutil/sysutil_screenshot.h>
#include <sysutil/sysutil_common.h>
#include <sysutil/sysutil_gamecontent.h>
#ifdef HAVE_HDD_CACHE_PARTITION
#include <sysutil/sysutil_syscache.h>
#endif
#endif

#if(CELL_SDK_VERSION > 0x340000)
#include <sysutil/sysutil_bgmplayback.h>
#endif

#ifdef HAVE_SYSMODULES
#include <cell/sysmodule.h>
#endif
#include <netex/net.h>
#include <np.h>
#include <np/drm.h>

#include "../ps3_input.h"

#include "../../gfx/gl_common.h"

#include "../../console/retroarch_console.h"
#include "../../conf/config_file.h"
#include "../../conf/config_file_macros.h"
#include "../../general.h"
#include "../../file.h"

#include "menu.h"

#define EMULATOR_CONTENT_DIR "SSNE10000"

#define CACHE_ID "ABCD12345"
#define NP_POOL_SIZE (128*1024)

static uint8_t np_pool[NP_POOL_SIZE];

int rarch_main(int argc, char *argv[]);

SYS_PROCESS_PARAM(1001, 0x200000)

#undef main

static void set_default_settings(void)
{
   // g_settings
   strlcpy(g_settings.cheat_database, default_paths.port_dir, sizeof(g_settings.cheat_database));
   g_settings.rewind_enable = false;
   strlcpy(g_settings.video.cg_shader_path, default_paths.shader_file, sizeof(g_settings.video.cg_shader_path));
   g_settings.video.fbo_scale_x = 2.0f;
   g_settings.video.fbo_scale_y = 2.0f;
   g_settings.video.render_to_texture = true;
   strlcpy(g_settings.video.second_pass_shader, default_paths.shader_file, sizeof(g_settings.video.second_pass_shader));
   g_settings.video.second_pass_smooth = true;
   g_settings.video.smooth = true;
   g_settings.video.vsync = true;
   strlcpy(g_settings.cheat_database, default_paths.port_dir, sizeof(g_settings.cheat_database));
   strlcpy(g_settings.system_directory, default_paths.system_dir, sizeof(g_settings.system_directory));
   g_settings.video.msg_pos_x = 0.05f;
   g_settings.video.msg_pos_y = 0.90f;
   g_settings.video.aspect_ratio = -1.0f;

   rarch_input_set_controls_default();

   // g_console
   g_console.block_config_read = true;
   g_console.frame_advance_enable = false;
   g_console.emulator_initialized = 0;
   g_console.screenshots_enable = true;
   g_console.throttle_enable = true;
   g_console.initialize_rarch_enable = false;
   g_console.triple_buffering_enable = true;
   g_console.default_savestate_dir_enable = false;
   g_console.default_sram_dir_enable = false;
   g_console.fbo_enabled = true;
   g_console.mode_switch = MODE_MENU;
   g_console.screen_orientation = ORIENTATION_NORMAL;
   g_console.current_resolution_id = 0;
   strlcpy(g_console.default_rom_startup_dir, "/", sizeof(g_console.default_rom_startup_dir));
   strlcpy(g_console.default_savestate_dir, default_paths.savestate_dir, sizeof(g_console.default_savestate_dir));
   strlcpy(g_console.default_sram_dir, default_paths.sram_dir, sizeof(g_console.default_sram_dir));
   g_console.aspect_ratio_index = 0;
   g_console.menu_font_size = 1.0f;
   g_console.overscan_enable = false;
   g_console.overscan_amount = 0.0f;
   g_console.sound_mode = SOUND_MODE_NORMAL;
   g_console.viewports.custom_vp.width = 0;
   g_console.viewports.custom_vp.height = 0;
   g_console.viewports.custom_vp.x = 0;
   g_console.viewports.custom_vp.y = 0;
   g_console.custom_bgm_enable = true;
   g_console.info_msg_enable = true;

   // g_extern
   g_extern.state_slot = 0;
   g_extern.audio_data.mute = 0;
   g_extern.verbose = true;
}

#ifdef HAVE_SYSUTILS
static void callback_sysutil_exit(uint64_t status, uint64_t param, void *userdata)
{
   (void) param;
   (void) userdata;
   gl_t *gl = driver.video_data;

   switch (status)
   {
      case CELL_SYSUTIL_REQUEST_EXITGAME:
	 gl->quitting = true;
         rarch_settings_change(S_QUIT);
	 break;
#ifdef HAVE_OSKUTIL
      case CELL_SYSUTIL_OSKDIALOG_FINISHED:
	 oskutil_close(&g_console.oskutil_handle);
	 oskutil_finished(&g_console.oskutil_handle);
	 break;
      case CELL_SYSUTIL_OSKDIALOG_UNLOADED:
	 oskutil_unload(&g_console.oskutil_handle);
	 break;
#endif
   }
}
#endif

static void get_environment_settings(int argc, char *argv[])
{
   g_extern.verbose = true;

   int ret;
   unsigned int get_type;
   unsigned int get_attributes;
   CellGameContentSize size;
   char dirName[CELL_GAME_DIRNAME_SIZE];
   char contentInfoPath[PATH_MAX];

#ifdef HAVE_HDD_CACHE_PARTITION
   CellSysCacheParam param;
   memset(&param, 0x00, sizeof(CellSysCacheParam));
   strlcpy(param.cacheId,CACHE_ID, sizeof(CellSysCacheParam));

   ret = cellSysCacheMount(&param);
   if(ret != CELL_SYSCACHE_RET_OK_CLEARED)
   {
      RARCH_ERR("System cache partition could not be mounted, it might be already mounted.\n");
   }
#endif

#ifdef HAVE_MULTIMAN
   if(argc > 1)
   {
      /* launched from external launcher */
      strlcpy(default_paths.multiman_self_file, argv[2], sizeof(default_paths.multiman_self_file));
   }
   else
   {
      /* not launched from external launcher, set default path */
      strlcpy(default_paths.multiman_self_file, "/dev_hdd0/game/BLES80608/USRDIR/RELOAD.SELF",
         sizeof(default_paths.multiman_self_file));
   }

   if(path_file_exists(default_paths.multiman_self_file) && argc > 1 &&  path_file_exists(argv[1]))
   {
      g_console.external_launcher_support = EXTERN_LAUNCHER_MULTIMAN;
      RARCH_LOG("Started from multiMAN, auto-game start enabled.\n");
   }
   else
#endif
   {
      g_console.external_launcher_support = EXTERN_LAUNCHER_SALAMANDER;
      RARCH_WARN("Not started from multiMAN, auto-game start disabled.\n");
   }

   memset(&size, 0x00, sizeof(CellGameContentSize));

   ret = cellGameBootCheck(&get_type, &get_attributes, &size, dirName);
   if(ret < 0)
   {
      RARCH_ERR("cellGameBootCheck() Error: 0x%x.\n", ret);
   }
   else
   {
      RARCH_LOG("cellGameBootCheck() OK.\n");
      RARCH_LOG("Directory name: [%s].\n", dirName);
      RARCH_LOG(" HDD Free Size (in KB) = [%d] Size (in KB) = [%d] System Size (in KB) = [%d].\n", size.hddFreeSizeKB, size.sizeKB, size.sysSizeKB);

      switch(get_type)
      {
         case CELL_GAME_GAMETYPE_DISC:
            RARCH_LOG("RetroArch was launched on Optical Disc Drive.\n");
	    break;
	 case CELL_GAME_GAMETYPE_HDD:
	    RARCH_LOG("RetroArch was launched on HDD.\n");
	    break;
      }

      if((get_attributes & CELL_GAME_ATTRIBUTE_APP_HOME) == CELL_GAME_ATTRIBUTE_APP_HOME)
         RARCH_LOG("RetroArch was launched from host machine (APP_HOME).\n");

      ret = cellGameContentPermit(contentInfoPath, default_paths.port_dir);

#ifdef HAVE_MULTIMAN
      if(g_console.external_launcher_support == EXTERN_LAUNCHER_MULTIMAN)
      {
         snprintf(contentInfoPath, sizeof(contentInfoPath), "/dev_hdd0/game/%s", EMULATOR_CONTENT_DIR);
	 snprintf(default_paths.port_dir, sizeof(default_paths.port_dir), "/dev_hdd0/game/%s/USRDIR", EMULATOR_CONTENT_DIR);
      }
#endif

      if(ret < 0)
      {
         RARCH_ERR("cellGameContentPermit() Error: 0x%x\n", ret);
      }
      else
      {
         RARCH_LOG("cellGameContentPermit() OK.\n");
	 RARCH_LOG("contentInfoPath : [%s].\n", contentInfoPath);
	 RARCH_LOG("usrDirPath : [%s].\n", default_paths.port_dir);
      }

      snprintf(default_paths.core_dir, sizeof(default_paths.core_dir), "%s/cores", default_paths.port_dir);
      snprintf(default_paths.savestate_dir, sizeof(default_paths.savestate_dir), "%s/savestates", default_paths.core_dir);
      snprintf(default_paths.sram_dir, sizeof(default_paths.sram_dir), "%s/sram", default_paths.core_dir);

      snprintf(default_paths.system_dir, sizeof(default_paths.system_dir), "%s/system", default_paths.core_dir);

      /* now we fill in all the variables */
      snprintf(default_paths.border_file, sizeof(default_paths.border_file), "%s/borders/Centered-1080p/mega-man-2.png", default_paths.core_dir);
      snprintf(default_paths.menu_border_file, sizeof(default_paths.menu_border_file), "%s/borders/Menu/main-menu.png", default_paths.core_dir);
      snprintf(default_paths.cgp_dir, sizeof(default_paths.cgp_dir), "%s/presets", default_paths.core_dir);
      snprintf(default_paths.input_presets_dir, sizeof(default_paths.input_presets_dir), "%s/input", default_paths.cgp_dir);
      snprintf(default_paths.border_dir, sizeof(default_paths.border_dir), "%s/borders", default_paths.core_dir);
      snprintf(default_paths.shader_dir, sizeof(default_paths.shader_dir), "%s/shaders", default_paths.core_dir);
      snprintf(default_paths.shader_file, sizeof(default_paths.shader_file), "%s/shaders/stock.cg", default_paths.core_dir);
      snprintf(default_paths.menu_shader_file, sizeof(default_paths.menu_shader_file), "%s/shaders/Borders/Menu/border-only-rarch.cg", default_paths.core_dir);
      snprintf(default_paths.config_file, sizeof(default_paths.config_file), "%s/retroarch.cfg", default_paths.port_dir);
   }

   g_extern.verbose = false;
}

int main(int argc, char *argv[])
{
#ifdef HAVE_SYSUTILS
   RARCH_LOG("Registering system utility callback...\n");
   cellSysutilRegisterCallback(0, callback_sysutil_exit, NULL);
#endif

#ifdef HAVE_SYSMODULES
   cellSysmoduleLoadModule(CELL_SYSMODULE_IO);
   cellSysmoduleLoadModule(CELL_SYSMODULE_FS);
   cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_GAME);
   cellSysmoduleLoadModule(CELL_SYSMODULE_AVCONF_EXT);
   cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);
   cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC);
   cellSysmoduleLoadModule(CELL_SYSMODULE_NET);
   cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_NP);
#endif

   sys_net_initialize_network();

#ifdef HAVE_LOGGER
   logger_init();
#endif

   sceNpInit(NP_POOL_SIZE, np_pool);

   rarch_main_clear_state();
   get_environment_settings(argc, argv);

   config_set_defaults();

   char full_path[1024], tmp_path[1024];
   snprintf(full_path, sizeof(full_path), "%s/CORE.SELF", default_paths.core_dir);
   snprintf(tmp_path, sizeof(tmp_path), "%s/", default_paths.core_dir);

   bool find_libretro_file = rarch_configure_libretro_core(full_path, tmp_path, default_paths.core_dir, 
   default_paths.config_file, ".SELF");

   set_default_settings();
   rarch_config_load(default_paths.config_file, default_paths.core_dir, ".SELF", find_libretro_file);
   init_libretro_sym();

#if(CELL_SDK_VERSION > 0x340000)
   if (g_console.screenshots_enable)
   {
#ifdef HAVE_SYSMODULES
      cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_SCREENSHOT);
#endif
#ifdef HAVE_SYSUTILS
      CellScreenShotSetParam screenshot_param = {0, 0, 0, 0};

      screenshot_param.photo_title = "RetroArch PS3";
      screenshot_param.game_title = "RetroArch PS3";
      cellScreenShotSetParameter (&screenshot_param);
      cellScreenShotEnable();
#endif
   }
#ifdef HAVE_SYSUTILS
   if (g_console.custom_bgm_enable)
      cellSysutilEnableBgmPlayback();
#endif
#endif

   video_gl.start();

   input_ps3.init();

#ifdef HAVE_OSKUTIL
   oskutil_init(&g_console.oskutil_handle, 0);
#endif

   rarch_input_set_default_keybind_names_for_emulator();

   menu_init();

   switch(g_console.external_launcher_support)
   {
      case EXTERN_LAUNCHER_SALAMANDER:
         g_console.mode_switch = MODE_MENU;
	 break;
#ifdef HAVE_MULTIMAN
      case EXTERN_LAUNCHER_MULTIMAN:
	 RARCH_LOG("Started from multiMAN, will auto-start game.\n");
	 strlcpy(g_console.rom_path, argv[1], sizeof(g_console.rom_path));
         rarch_settings_change(S_START_RARCH);
	 rarch_startup(default_paths.config_file);
	 break;
#endif
      default:
         break;
   }

begin_loop:
   if(g_console.mode_switch == MODE_EMULATION)
   {
      bool repeat = false;

      input_ps3.poll(NULL);

      rarch_set_auto_viewport(g_extern.frame_cache.width, g_extern.frame_cache.height);

      do{
         repeat = rarch_main_iterate();
      }while(repeat && !g_console.frame_advance_enable);
   }
   else if(g_console.mode_switch == MODE_MENU)
   {
      menu_loop();
      rarch_startup(default_paths.config_file);
   }
   else
      goto begin_shutdown;

   goto begin_loop;

begin_shutdown:
   if(path_file_exists(default_paths.config_file))
      rarch_config_save(default_paths.config_file);

   if(g_console.emulator_initialized)
      rarch_main_deinit();

   input_ps3.free(NULL);
   video_gl.stop();
   menu_free();

#ifdef HAVE_OSKUTIL
   if(g_console.oskutil_handle.is_running)
      oskutil_unload(&g_console.oskutil_handle);
#endif

#ifdef HAVE_LOGGER
   logger_shutdown();
#endif

#ifdef HAVE_SYSMODULES
   if(g_console.screenshots_enable)
      cellSysmoduleUnloadModule(CELL_SYSMODULE_SYSUTIL_SCREENSHOT);
   cellSysmoduleUnloadModule(CELL_SYSMODULE_JPGDEC);
   cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);
   cellSysmoduleUnloadModule(CELL_SYSMODULE_AVCONF_EXT);
   cellSysmoduleUnloadModule(CELL_SYSMODULE_SYSUTIL_GAME);
#endif

#ifdef HAVE_HDD_CACHE_PARTITION
   int ret = cellSysCacheClear();

   if(ret != CELL_SYSCACHE_RET_OK_CLEARED)
   {
      RARCH_ERR("System cache partition could not be cleared on exit.\n");
   }
#endif

   rarch_exec();

   return 1;
}
