@ECHO OFF
SETLOCAL ENABLEDELAYEDEXPANSION
rem ----Usage----
rem BuildSetup [gl|dx] [clean|noclean]
rem vs2010 for compiling with visual studio 2010
rem gl for opengl build
rem dx for directx build (default)
rem clean to force a full rebuild
rem noclean to force a build without clean
rem noprompt to avoid all prompts
rem nomingwlibs to skip building all libs built with mingw
CLS
COLOR 1B
TITLE XBMC for Windows Build Script
rem ----PURPOSE----
rem - Create a working XBMC build with a single click
rem -------------------------------------------------------------
rem Config
rem If you get an error that Visual studio was not found, SET your path for VSNET main executable.
rem -------------------------------------------------------------
rem	CONFIG START
SET target=dx
SET buildmode=ask
SET promptlevel=prompt
SET buildmingwlibs=true
SET exitcode=0
SET useshell=rxvt
SET BRANCH=na
FOR %%b in (%1, %2, %3, %4, %5) DO (
	IF %%b==dx SET target=dx
	IF %%b==gl SET target=gl
	IF %%b==clean SET buildmode=clean
	IF %%b==noclean SET buildmode=noclean
	IF %%b==noprompt SET promptlevel=noprompt
	IF %%b==nomingwlibs SET buildmingwlibs=false
	IF %%b==sh SET useshell=sh
)

SET buildconfig=Release (DirectX)
set WORKSPACE=%CD%\..\..


  REM look for MSBuild.exe delivered with Visual Studio 2013
  FOR /F "tokens=2,* delims= " %%A IN ('REG QUERY HKLM\SOFTWARE\Microsoft\MSBuild\ToolsVersions\12.0 /v MSBuildToolsRoot') DO SET MSBUILDROOT=%%B
  SET NET="%MSBUILDROOT%12.0\bin\MSBuild.exe"

  IF EXIST "!NET!" (
	set msbuildemitsolution=1
	set OPTS_EXE="..\VS2010Express\XBMC for Windows.sln" /t:Build /p:Configuration="%buildconfig%" /property:VCTargetsPath="%MSBUILDROOT%Microsoft.Cpp\v4.0\V120"
	set CLEAN_EXE="..\VS2010Express\XBMC for Windows.sln" /t:Clean /p:Configuration="%buildconfig%" /property:VCTargetsPath="%MSBUILDROOT%Microsoft.Cpp\v4.0\V120"
  )

  IF NOT EXIST %NET% (
    set DIETEXT=MSBuild was not found.
	goto DIE
  )
  
  set EXE= "..\VS2010Express\XBMC\%buildconfig%\XBMC.exe"
  set PDB= "..\VS2010Express\XBMC\%buildconfig%\XBMC.pdb"
  
  :: sets the BRANCH env var
  call getbranch.bat

  rem	CONFIG END
  rem -------------------------------------------------------------

  echo                         :                                                  
  echo                        :::                                                 
  echo                        ::::                                                
  echo                        ::::                                                
  echo    :::::::       :::::::::::::::::        ::::::      ::::::        :::::::
  echo    :::::::::   ::::::::::::::::::::     ::::::::::  ::::::::::    :::::::::
  echo     ::::::::: ::::::::::::::::::::::   ::::::::::::::::::::::::  ::::::::: 
  echo          :::::::::     :::      ::::: :::::    ::::::::    :::: :::::      
  echo           ::::::      ::::       :::: ::::      :::::       :::::::        
  echo           :::::       ::::        :::::::       :::::       ::::::         
  echo           :::::       :::         ::::::         :::        ::::::         
  echo           ::::        :::         ::::::        ::::        ::::::         
  echo           ::::        :::        :::::::        ::::        ::::::         
  echo          :::::        ::::       :::::::        ::::        ::::::         
  echo         :::::::       ::::      ::::::::        :::         :::::::        
  echo     :::::::::::::::    :::::  ::::: :::         :::         :::::::::      
  echo  :::::::::  :::::::::  :::::::::::  :::         :::         ::: :::::::::  
  echo  ::::::::    :::::::::  :::::::::   :::         :::         :::  ::::::::  
  echo ::::::         :::::::    :::::     :            ::          ::    ::::::  
  goto EXE_COMPILE

:EXE_COMPILE
  IF EXIST buildlog.html del buildlog.html /q
  IF NOT %buildmode%==ask goto COMPILE_MINGW
  IF %promptlevel%==noprompt goto COMPILE_MINGW
  rem ---------------------------------------------
  rem	check for existing exe
  rem ---------------------------------------------
  set buildmode=clean
  
  IF NOT EXIST %EXE% goto COMPILE_MINGW
  
  ECHO ------------------------------------------------------------
  ECHO Found a previous Compiled WIN32 EXE!
  ECHO [1] a NEW EXE will be compiled for the BUILD_WIN32
  ECHO [2] existing EXE will be updated (quick mode compile) for the BUILD_WIN32
  ECHO ------------------------------------------------------------
  set /P XBMC_COMPILE_ANSWER=Compile a new EXE? [1/2]:
  if /I %XBMC_COMPILE_ANSWER% EQU 1 set buildmode=clean
  if /I %XBMC_COMPILE_ANSWER% EQU 2 set buildmode=noclean

  goto COMPILE_MINGW
  

:COMPILE_MINGW
  ECHO Buildmode = %buildmode%
  IF %buildmingwlibs%==true (
    ECHO Compiling mingw libs
    ECHO bla>noprompt
    IF EXIST errormingw del errormingw > NUL
	IF %buildmode%==clean (
	  ECHO bla>makeclean
	)
    rem only use sh to please jenkins
    IF %useshell%==sh (
      call ..\..\tools\buildsteps\win32\make-mingwlibs.bat sh noprompt
    ) ELSE (
      call ..\..\tools\buildsteps\win32\make-mingwlibs.bat noprompt
    )
    IF EXIST errormingw (
    	set DIETEXT="failed to build mingw libs"
    	goto DIE
    )
  )
  IF %buildmode%==clean goto COMPILE_EXE
  goto COMPILE_NO_CLEAN_EXE
  
  
:COMPILE_EXE
  ECHO Wait while preparing the build.
  ECHO ------------------------------------------------------------
  ECHO Cleaning Solution...
  %NET% %CLEAN_EXE%
  ECHO Compiling XBMC branch %BRANCH%...
  %NET% %OPTS_EXE%
  IF %errorlevel%==1 (
  	set DIETEXT="XBMC.EXE failed to build!  See %CD%\..\vs2010express\XBMC\%buildconfig%\objs\XBMC.log"
	IF %promptlevel%==noprompt (
		type "%CD%\..\vs2010express\XBMC\%buildconfig%\objs\XBMC.log"
	)
  	goto DIE
  )
  ECHO Done!
  ECHO ------------------------------------------------------------
  set buildmode=clean
  GOTO MAKE_BUILD_EXE
  
:COMPILE_NO_CLEAN_EXE
  ECHO Wait while preparing the build.
  ECHO ------------------------------------------------------------
  ECHO Compiling XBMC branch %BRANCH%...
  %NET% %OPTS_EXE%
  IF %errorlevel%==1 (
  	set DIETEXT="XBMC.EXE failed to build!  See %CD%\..\vs2010express\XBMC\%buildconfig%\objs\XBMC.log"
	IF %promptlevel%==noprompt (
		type "%CD%\..\vs2010express\XBMC\%buildconfig%\objs\XBMC.log"
	)
  	goto DIE
  )
  ECHO Done!
  ECHO ------------------------------------------------------------
  GOTO MAKE_BUILD_EXE

:MAKE_BUILD_EXE
  ECHO Copying files...
  IF EXIST BUILD_WIN32 rmdir BUILD_WIN32 /S /Q
  rem Add files to exclude.txt that should not be included in the installer
  
  Echo Thumbs.db>>exclude.txt
  Echo Desktop.ini>>exclude.txt
  Echo dsstdfx.bin>>exclude.txt
  Echo exclude.txt>>exclude.txt
  Echo xbmc.log>>exclude.txt
  Echo xbmc.old.log>>exclude.txt
  rem Exclude userdata files
  Echo userdata\advancedsettings.xml>>exclude.txt
  Echo userdata\guisettings.xml>>exclude.txt
  Echo userdata\mediasources.xml>>exclude.txt
  Echo userdata\passwords.xml>>exclude.txt
  Echo userdata\profiles.xml>>exclude.txt
  Echo userdata\sources.xml>>exclude.txt
  Echo userdata\upnpserver.xml>>exclude.txt
  rem Exclude userdata folders
  Echo userdata\addon_data\>>exclude.txt
  Echo userdata\cache\>>exclude.txt
  Echo userdata\database\>>exclude.txt
  Echo userdata\playlists\>>exclude.txt
  Echo userdata\thumbnails\>>exclude.txt
  rem Exclude non Windows addons
  Echo addons\repository.pvr-android.xbmc.org\>>exclude.txt
  Echo addons\repository.pvr-ios.xbmc.org\>>exclude.txt
  Echo addons\repository.pvr-osx32.xbmc.org\>>exclude.txt
  Echo addons\repository.pvr-osx64.xbmc.org\>>exclude.txt
  Echo addons\screensaver.rsxs.euphoria\>>exclude.txt
  Echo addons\screensaver.rsxs.plasma\>>exclude.txt
  Echo addons\screensaver.rsxs.solarwinds\>>exclude.txt
  Echo addons\visualization.fishbmc\>>exclude.txt
  Echo addons\visualization.projectm\>>exclude.txt
  Echo addons\visualization.glspectrum\>>exclude.txt
  rem Exclude skins as they're copied by their own script
  Echo addons\skin.touched\>>exclude.txt
  Echo addons\skin.confluence\>>exclude.txt
  rem other platform stuff
  Echo lib-osx>>exclude.txt
  Echo players\mplayer>>exclude.txt
  Echo FileZilla Server.xml>>exclude.txt
  Echo asound.conf>>exclude.txt
  Echo voicemasks.xml>>exclude.txt
  Echo Lircmap.xml>>exclude.txt
  
  md BUILD_WIN32\Xbmc

  xcopy %EXE% BUILD_WIN32\Xbmc > NUL
  xcopy ..\..\userdata BUILD_WIN32\Xbmc\userdata /E /Q /I /Y /EXCLUDE:exclude.txt > NUL
  copy ..\..\copying.txt BUILD_WIN32\Xbmc > NUL
  copy ..\..\LICENSE.GPL BUILD_WIN32\Xbmc > NUL
  copy ..\..\known_issues.txt BUILD_WIN32\Xbmc > NUL
  xcopy dependencies\*.* BUILD_WIN32\Xbmc /Q /I /Y /EXCLUDE:exclude.txt  > NUL
  
  xcopy ..\..\language BUILD_WIN32\Xbmc\language /E /Q /I /Y /EXCLUDE:exclude.txt  > NUL
  xcopy ..\..\addons BUILD_WIN32\Xbmc\addons /E /Q /I /Y /EXCLUDE:exclude.txt > NUL
  xcopy ..\..\system BUILD_WIN32\Xbmc\system /E /Q /I /Y /EXCLUDE:exclude.txt  > NUL
  xcopy ..\..\media BUILD_WIN32\Xbmc\media /E /Q /I /Y /EXCLUDE:exclude.txt  > NUL
  xcopy ..\..\sounds BUILD_WIN32\Xbmc\sounds /E /Q /I /Y /EXCLUDE:exclude.txt  > NUL
  
  ECHO ------------------------------------------------------------
  call buildpvraddons.bat
  IF %errorlevel%==1 (
    set DIETEXT="failed to build pvr addons"
    goto DIE
  )
    
  IF EXIST error.log del error.log > NUL
  SET build_path=%CD%
  ECHO ------------------------------------------------------------
  ECHO Building Confluence Skin...
  cd ..\..\addons\skin.confluence
  call build.bat > NUL
  cd %build_path%
  
  IF EXIST  ..\..\addons\skin.touched\build.bat (
    ECHO Building Touched Skin...
    cd ..\..\addons\skin.touched
    call build.bat > NUL
    cd %build_path%
  )
  
  rem restore color and title, some scripts mess these up
  COLOR 1B
  TITLE XBMC for Windows Build Script

  IF EXIST exclude.txt del exclude.txt  > NUL
  del /s /q /f BUILD_WIN32\Xbmc\*.so  > NUL
  del /s /q /f BUILD_WIN32\Xbmc\*.h  > NUL
  del /s /q /f BUILD_WIN32\Xbmc\*.cpp  > NUL
  del /s /q /f BUILD_WIN32\Xbmc\*.exp  > NUL
  del /s /q /f BUILD_WIN32\Xbmc\*.lib  > NUL
  
  ECHO ------------------------------------------------------------
  ECHO Build Succeeded!
  GOTO NSIS_EXE

:NSIS_EXE
  ECHO ------------------------------------------------------------
  ECHO Generating installer includes...
  call genNsisIncludes.bat
  ECHO ------------------------------------------------------------
  call getdeploydependencies.bat
  CALL extract_git_rev.bat > NUL
  SET XBMC_SETUPFILE=XBMCSetup-%GIT_REV%-%BRANCH%.exe
  SET XBMC_PDBFILE=XBMCSetup-%GIT_REV%-%BRANCH%.pdb
  ECHO Creating installer %XBMC_SETUPFILE%...
  IF EXIST %XBMC_SETUPFILE% del %XBMC_SETUPFILE% > NUL
  rem get path to makensis.exe from registry, first try tab delim
  FOR /F "tokens=2* delims=	" %%A IN ('REG QUERY "HKLM\Software\NSIS" /ve') DO SET NSISExePath=%%B

  IF NOT EXIST "%NSISExePath%" (
    rem try with space delim instead of tab
    FOR /F "tokens=2* delims= " %%A IN ('REG QUERY "HKLM\Software\NSIS" /ve') DO SET NSISExePath=%%B
  )
      
  IF NOT EXIST "%NSISExePath%" (
    rem fails on localized windows (Default) becomes (Par D�faut)
    FOR /F "tokens=3* delims=	" %%A IN ('REG QUERY "HKLM\Software\NSIS" /ve') DO SET NSISExePath=%%B
  )

  IF NOT EXIST "%NSISExePath%" (
    FOR /F "tokens=3* delims= " %%A IN ('REG QUERY "HKLM\Software\NSIS" /ve') DO SET NSISExePath=%%B
  )
  
  rem proper x64 registry checks
  IF NOT EXIST "%NSISExePath%" (
    ECHO using x64 registry entries
    FOR /F "tokens=2* delims=	" %%A IN ('REG QUERY "HKLM\Software\Wow6432Node\NSIS" /ve') DO SET NSISExePath=%%B
  )
  IF NOT EXIST "%NSISExePath%" (
    rem try with space delim instead of tab
    FOR /F "tokens=2* delims= " %%A IN ('REG QUERY "HKLM\Software\Wow6432Node\NSIS" /ve') DO SET NSISExePath=%%B
  )
  IF NOT EXIST "%NSISExePath%" (
    rem on win 7 x64, the previous fails
    FOR /F "tokens=3* delims=	" %%A IN ('REG QUERY "HKLM\Software\Wow6432Node\NSIS" /ve') DO SET NSISExePath=%%B
  )
  IF NOT EXIST "%NSISExePath%" (
    rem try with space delim instead of tab
    FOR /F "tokens=3* delims= " %%A IN ('REG QUERY "HKLM\Software\Wow6432Node\NSIS" /ve') DO SET NSISExePath=%%B
  )

  SET NSISExe=%NSISExePath%\makensis.exe
  "%NSISExe%" /V1 /X"SetCompressor /FINAL lzma" /Dxbmc_root="%CD%\BUILD_WIN32" /Dxbmc_revision="%GIT_REV%" /Dxbmc_target="%target%" /Dxbmc_branch="%BRANCH%" "XBMC for Windows.nsi"
  IF NOT EXIST "%XBMC_SETUPFILE%" (
	  set DIETEXT=Failed to create %XBMC_SETUPFILE%. NSIS installed?
	  goto DIE
  )
  copy %PDB% %XBMC_PDBFILE% > nul
  ECHO ------------------------------------------------------------
  ECHO Done!
  ECHO Setup is located at %CD%\%XBMC_SETUPFILE%
  ECHO ------------------------------------------------------------
  GOTO VIEWLOG_EXE
  
:DIE
  ECHO ------------------------------------------------------------
  ECHO !-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-
  ECHO    ERROR ERROR ERROR ERROR ERROR ERROR ERROR ERROR ERROR
  ECHO !-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-!-
  set DIETEXT=ERROR: %DIETEXT%
  echo %DIETEXT%
  SET exitcode=1
  ECHO ------------------------------------------------------------
  GOTO END

:VIEWLOG_EXE
  SET log="%CD%\..\vs2010express\XBMC\%buildconfig%\objs\XBMC.log"
  IF NOT EXIST %log% goto END
  
  copy %log% ./buildlog.html > NUL

  IF %promptlevel%==noprompt (
  goto END
  )

  set /P XBMC_BUILD_ANSWER=View the build log in your HTML browser? [y/n]
  if /I %XBMC_BUILD_ANSWER% NEQ y goto END
  
  SET log="%CD%\..\vs2010express\XBMC\%buildconfig%\objs\" XBMC.log
  
  start /D%log%
  goto END

:END
  IF %promptlevel% NEQ noprompt (
  ECHO Press any key to exit...
  pause > NUL
  )
  EXIT /B %exitcode%
