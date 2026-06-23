#
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
# distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

import glob
import os

Import('*')

env = SConscript('Compilers/Clang.sconscript.py')


vars = Variables()
vars.AddVariables(
    ('architecture', 'Sets the target build architecture, must be ARM64 or x64.')
)
Help(vars.GenerateHelpText(Environment()))

architecture = ARGUMENTS.get('architecture', 'ARM64')
if architecture not in ['ARM64', 'x64']:
    print('Error: invalid build architecture')
    Exit(1)


# Find the latest macOS SDK
macOSSDKPath = '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
if not os.path.isdir(macOSSDKPath):
    oldSDKPrefix = '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.'
    paths = glob.glob(oldSDKPrefix + '*')
    if paths:
        paths = sorted(paths, key=lambda path: int(path[len(oldSDKPrefix):-4]))
        macOSSDKPath = paths[-1]
    else:
        print('Error: failed finding macOS SDK, check that Xcode is installed')
        Exit(1)

# Setup environment for the selected SDK
flags = ['-arch', {'x64': 'x86_64', 'ARM64': 'arm64'}[architecture],
         '-mmacosx-version-min=11.0', '-isysroot', macOSSDKPath]

env['CCFLAGS'] += flags + ['-fobjc-arc']
env['LINKFLAGS'] += flags + ['-Wl,-syslibroot,' + macOSSDKPath]


# The SetupForLinkingCarbon() method sets up the environment for linking Carbon as a dynamic library or linking Carbon
# as a static library into a final application
def SetupForLinkingCarbon(self, **keywords):

    defaultDependencies = ['AngelScript', 'Bullet', 'FreeImage', 'FreeType', 'OpenAssetImport', 'Vorbis', 'ZLib']

    if architecture == 'x64':
        defaultDependencies.append('PhysX')
        
    dependencies = keywords.get('dependencies', defaultDependencies)

    self['LIBPATH'] += GetDependencyLIBPATH(*dependencies)
    self['LIBS'] += ['iconv'] + dependencies
    self['FRAMEWORKS'] += ['Cocoa', 'GameKit', 'IOKit', 'OpenAL', 'OpenGL', 'StoreKit']

env.AddMethod(SetupForLinkingCarbon)


# Add a method for setting up an environment ready for building against the installed SDK
def Carbonize(self, **keywords):
    if self.IsCarbonEngineStatic():
        self['CPPDEFINES'] += ['CARBON_STATIC_LIBRARY']

    self['LIBS'] += ['CarbonEngine' + {'Debug': 'Debug', 'Release': ''}[buildType]]

    if 'carbonroot' in ARGUMENTS:
        self['CPPPATH'] += [os.path.join(ARGUMENTS['carbonroot'], 'Source')]
        self['LIBPATH'] += [os.path.join(ARGUMENTS['carbonroot'], 'Build', 'macOS', architecture, 'Clang', buildType)]

        if self.IsCarbonEngineStatic():
            self.SetupForLinkingCarbon()
    else:
        self['CPPPATH'] += ['/Applications/Carbon SDK/Include']
        self['LIBPATH'] += ['/Applications/Carbon SDK/Library']

        if self.IsCarbonEngineStatic():
            self.SetupForLinkingCarbon(dependencies=[])

    self.Append(**keywords)
    self.SetupPrecompiledHeader(keywords)

env.AddMethod(Carbonize)

# Return the build details
details = {'platform': 'macOS', 'architecture': architecture, 'compiler': 'Clang', 'env': env}

Return('details')
