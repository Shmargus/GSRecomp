# Keep one list of build processes. Every build and release script checks
# it because overlapping builds can corrupt libraries and leave an empty
# executable.
$GsrBuildProcessNames = @('cmake', 'ninja', 'make', 'mingw32-make', 'gcc', 'g++', 'cc', 'c++',
                          'cc1', 'cc1plus', 'lto1', 'lto-wrapper', 'ld', 'collect2', 'gsr_builder')

function Get-GsrBuildProcesses {
    @(Get-Process -Name $GsrBuildProcessNames -ErrorAction SilentlyContinue)
}
