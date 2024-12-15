#!/bin/bash

if [ "${GITHUB_ACTIONS}" = "true" ] ; then
  # CI is already set
  export CI_BRANCH="$(echo ${GITHUB_REF} | cut -d/ -f3)"
  if [ "${GITHUB_EVENT_NAME}" = "push" ] && [ "${GITHUB_REPOSITORY_OWNER}" = "dosemu2" ] && [ "${CI_BRANCH}" = "devel" ] ; then
    export RUNTYPE="simple"
  fi
fi

TBINS="test-binaries"
if [ "${CI}" = "true" ] ; then
  [ -d "${HOME}"/cache ] || mkdir "${HOME}"/cache
  [ -h "${TBINS}" ] || ln -s "${HOME}"/cache "${TBINS}"
else
  [ -d "${TBINS}"] || mkdir "${TBINS}"
fi
python3 test/test_dos.py --get-test-binaries

# Make cpu tests here so that we see any failures
make -C test/cpu clean all

echo
echo "====================================================="
echo "=        Tests run on various flavours of DOS       ="
echo "====================================================="
# all DOS flavours, all tests
# python3 test/test_dos.py
# single DOS example
# python3 test/test_dos.py FRDOS120TestCase
# single test example
# python3 test/test_dos.py FRDOS120TestCase.test_mfs_fcb_rename_wild_1

export PYTHONUNBUFFERED=1
export TEST_DOSEMU=/usr/local/bin/dosemu
export TEST_CMDDIR=/usr/local/share/dosemu/commands

case "${RUNTYPE}" in
  "packaged")
    export NO_FAILFAST=1
    export SKIP_EXPENSIVE=1
    export SKIP_UNCERTAIN=1
    if [ "${OS}" = "ubuntu-20.04" -o "${OS}" = "ubuntu-22.04" ] ; then
      export SKIP_NATIVE_DPMI=1
    fi
    export TEST_DOSEMU=/usr/bin/dosemu
    export TEST_CMDDIR=/usr/share/dosemu/dosemu2-cmds-0.3
    python3 test/test_dos.py PPDOSGITTestCase
    python3 test/test_dos.py MSDOS622TestCase
    python3 test/test_dos.py FRDOS130TestCase
    python3 test/test_dos.py DRDOS701TestCase
    ;;
  "full")
    export NO_FAILFAST=1
    python3 test/test_dos.py PPDOSGITTestCase
    python3 test/test_dos.py MSDOS622TestCase
    python3 test/test_dos.py FRDOS130TestCase
    python3 test/test_dos.py DRDOS701TestCase
    ;;
  "normal")
    export SKIP_UNCERTAIN=1
    python3 test/test_dos.py PPDOSGITTestCase
    python3 test/test_dos.py MSDOS622TestCase
    ;;
  "simple")
    export SKIP_EXPENSIVE=1
    export SKIP_UNCERTAIN=1
    python3 test/test_dos.py PPDOSGITTestCase
    ;;
esac

for i in test_*.*.*.log ; do
  test -f $i || exit 0
done

# If we get here, then we've failed so copy various system logs, give them
# a name that is picked up by the artefact uploaded.
[ -f /var/log/audit/audit.log ] && sudo cp /var/log/audit/audit.log test_audit.log
[ -f /var/log/syslog ] && sudo cp /var/log/syslog test_syslog.log
[ -f /var/log/auth.log ] && sudo cp /var/log/auth.log test_auth.log

sudo chmod 644 test_*.log

exit 1
