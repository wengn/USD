#!/pxrpythonsubst
#
# Copyright 2018 Pixar
#
# Licensed under the Apache License, Version 2.0 (the "Apache License")
# with the following modification; you may not use this file except in
# compliance with the Apache License and the following modification to it:
# Section 6. Trademarks. is deleted and replaced with:
#
# 6. Trademarks. This License does not grant permission to use the trade
#    names, trademarks, service marks, or product names of the Licensor
#    and its affiliates, except as required to comply with Section 4(c) of
#    the License and to reproduce the content of the NOTICE file.
#
# You may obtain a copy of the Apache License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the Apache License with the above modification is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied. See the Apache License for the specific
# language governing permissions and limitations under the Apache License.
#
import argparse
import glob 
import os
import sys

from pxr import Ar, Sdf, Tf, Usd, UsdUtils
from contextlib import contextmanager

@contextmanager
def _Stream(path, *args, **kwargs):
    if path == '-':
        yield sys.stdout
    else:
        with open(path, *args, **kwargs) as fp:
            yield fp

def _Print(stream, msg):
    print >>stream, msg

def _Err(msg):
    sys.stderr.write(msg + '\n')

def _CheckCompliance(rootLayer, arkit=False):
    checker = UsdUtils.ComplianceChecker(rootLayer, arkit=arkit, 
        # We're going to flatten the USD stage and convert the root layer to 
        # crate file format before packaging, if necessary. Hence, skip these 
        # checks.
        skipARKitRootLayerCheck=True)
    
    errors = checker.GetErrors()
    failedChecks = checker.GetFailedChecks()
    for msg in errors + failedChecks:
        _Err(msg)
    return len(errors) == 0 and len(failedChecks) == 0

def _CreateUsdzPackage(usdzFile, filesToAdd, recurse, checkCompliance, verbose):
    with Usd.ZipFileWriter.CreateNew(usdzFile) as usdzWriter:
        fileList = []
        while filesToAdd:
            # Pop front (first file) from the list of files to add.
            f = filesToAdd[0]
            filesToAdd = filesToAdd[1:]

            if os.path.isdir(f):
                # If a directory is specified, add all files in the directory.
                filesInDir = glob.glob(os.path.join(f, '*'))
                # If the recurse flag is not specified, remove sub-directories.
                if not recurse:
                    filesInDir = [f for f in filesInDir if not os.path.isdir(f)]
                # glob.glob returns files in arbitrary order. Hence, sort them
                # here to get consistent ordering of files in the package.
                filesInDir.sort()
                filesToAdd += filesInDir
            else:
                if verbose:
                    print('.. adding: %s' % f)
                if os.path.getsize(f) > 0:
                    fileList.append(f)
                else:
                    _Err("Skipping empty file '%s'." % f)

        if checkCompliance and len(fileList) > 0:
            rootLayer = fileList[0]
            if not _CheckCompliance(rootLayer):
                return False

        for f in fileList:
            try:
                usdzWriter.AddFile(f)
            except Tf.ErrorException as e:
                _Err('Failed to add file \'%s\' to package. Discarding '
                    'package.' % f)
                # When the "with" block exits, Discard() will be called on 
                # usdzWriter automatically if an exception occurs.
                raise
        return True

def _DumpContents(dumpLocation, zipFile):
    with _Stream(dumpLocation, "w") as ofp:
        _Print(ofp, "    Offset\t      Comp\t    Uncomp\tName")
        _Print(ofp, "    ------\t      ----\t    ------\t----")
        fileNames = zipFile.GetFileNames()
        for fileName in fileNames:
            fileInfo = zipFile.GetFileInfo(fileName)
            _Print(ofp, "%10d\t%10d\t%10d\t%s" % 
                (fileInfo.dataOffset, fileInfo.size, 
                    fileInfo.uncompressedSize, fileName))

        _Print(ofp, "----------")
        _Print(ofp, "%d files total" % len(fileNames))

def _ListContents(listLocation, zipFile):
    with _Stream(listLocation, "w") as ofp:
        for fileName in zipFile.GetFileNames():
            _Print(ofp, fileName)

def main():
    parser = argparse.ArgumentParser(description='Utility for creating a .usdz '
        'file containging USD assets and for inspecting existing .usdz files.')

    parser.add_argument('usdzFile', type=str, 
                        help='Name of the .usdz file to create.')

    parser.add_argument('inputFiles', type=str, nargs='*',
                        help='Files to include in the .usdz file.')
    parser.add_argument('-r', '--recurse', dest='recurse', action='store_true',
                        help='If specified, files in sub-directories are '
                        'recursively added to the package.')

    parser.add_argument('-a', '--asset', dest='asset', type=str,
                        help='Resolvable asset path pointing to the root layer '
                        'of the asset to be isolated and copied into the '
                        'package.')
    parser.add_argument("--arkitAsset", dest="arkitAsset", type=str,
                        help="Similar to the --asset option, the --arkitAsset "
                        "option packages all of the dependencies of the named "
                        "scene file.  Assets targeted at the initial usdz "
                        "implementation in ARKit operate under greater "
                        "constraints than usdz files for more general 'in "
                        "house' uses, and this option attempts to ensure that "
                        "these constraints are honored; this may involve more "
                        "transformations to the data, which may cause loss of "
                        "features such as VariantSets.")

    parser.add_argument('-c', '--checkCompliance', dest='checkCompliance', 
                        action='store_true', help='Perform compliance checking '
                        'of the input files. If the input asset or \"root\" '
                        'layer fails any of the compliance checks, the package '
                        'is not created and the program fails.')

    parser.add_argument('-l', '--list', dest='listContents', type=str, 
                        nargs='?', default=None, const='-',
                        help='List contents of the specified usdz file. If '
                        'a file-path argument is provided, the list is output '
                        'to a file at the given path. If not, it is output to '
                        'stdout.')
    parser.add_argument('-d', '--dump', dest='dumpContents', type=str, 
                        nargs='?', default=None, const='-',
                        help='Dump contents of the specified usdz file. If '
                        'a file-path argument is provided, the contents are '
                        'output to a file at the given path. If not, it is '
                        'output to stdout.')

    parser.add_argument('-v', '--verbose', dest='verbose', action='store_true',
                        help='Enable verbose mode, which causes messages '
                        'regarding files being added to the package to be '
                        'output to stdout.')

    args = parser.parse_args()
    usdzFile = args.usdzFile
    filesToAdd = args.inputFiles

    if args.asset and args.arkitAsset:
        parser.error("Specify either --asset or --arkitAsset, not both.")

    if args.asset and len(filesToAdd)>0:
        parser.error("Specify either inputFiles or an asset via --asset, "
                     "not both.")

    # Ensure that the usdz file has the right extension.
    if not usdzFile.endswith('.usdz'):
        usdzFile += '.usdz'

    # Check if we're in package creation mode and verbose mode is enabled, 
    # print some useful information.
    if (args.asset or args.arkitAsset or len(filesToAdd)>0):
        if args.verbose:
            if os.path.exists(usdzFile):
                print("File at path '%s' already exists. Overwriting file." % 
                        usdzFile)

            print('Creating package \'%s\' with files [%s].' % 
                    (usdzFile, filesToAdd))
            if not args.recurse:
                print('Not recursing into sub-directories.')
    else:
        if args.checkCompliance:
            parser.error("--checkCompliance should only be specified when "
                "creatinga usdz package. Please use 'usdchecker' to check "
                "compliance of an existing .usdz file.")

    success = True
    if len(filesToAdd) > 0:
        success = _CreateUsdzPackage(usdzFile, filesToAdd, args.recurse, 
                args.checkCompliance, args.verbose) and success

    elif args.asset:
        r = Ar.GetResolver()
        resolvedAsset = r.Resolve(args.asset)
        if args.checkCompliance:
            success = _CheckCompliance(resolvedAsset, arkit=False) and success

        context = r.CreateDefaultContextForAsset(resolvedAsset) 
        with Ar.ResolverContextBinder(context):
            # Create the package only if the compliance check was passed.
            success = success and UsdUtils.CreateNewUsdzPackage(
                Sdf.AssetPath(args.asset), usdzFile)
    elif args.arkitAsset:
        r = Ar.GetResolver()
        resolvedAsset = r.Resolve(args.arkitAsset)
        if args.checkCompliance:
            success = _CheckCompliance(resolvedAsset, arkit=True) and success

        context = r.CreateDefaultContextForAsset(resolvedAsset)
        with Ar.ResolverContextBinder(context):
            # Create the package only if the compliance check was passed.
            success = success and UsdUtils.CreateNewARKitUsdzPackage(
                    Sdf.AssetPath(args.arkitAsset), usdzFile)

    if args.listContents or args.dumpContents:
        if os.path.exists(usdzFile):
            zipFile = Usd.ZipFile.Open(usdzFile)
            if zipFile:
                if args.dumpContents:
                    _DumpContents(args.dumpContents, zipFile)
                if args.listContents:
                    _ListContents(args.listContents, zipFile)
            else:
                _Err("Failed to open usdz file at path '%s'." % usdzFile)
        else:
            _Err("Can't find usdz file at path '%s'." % usdzFile)

    return 0 if success else 1

if __name__ == '__main__':
    sys.exit(main())
