#!/usr/bin/env python3
"""Exercise private-checkout updates against a local Git remote."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import update
import vcs_disc


class GitUpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.remote = base / 'remote'
        self.local = base / 'local'
        self.cmd('init', '-b', 'main', str(self.remote), cwd=base)
        self.cmd('config', 'user.email', 'test@example.invalid', cwd=self.remote)
        self.cmd('config', 'user.name', 'Test', cwd=self.remote)
        (self.remote / 'project.txt').write_text('initial', encoding='utf-8')
        self.cmd('add', 'project.txt', cwd=self.remote)
        self.cmd('commit', '-m', 'initial', cwd=self.remote)
        self.cmd('clone', str(self.remote), str(self.local), cwd=base)
        self.patch_root = mock.patch.object(update.tc, 'ROOT', str(self.local))
        self.patch_remote = mock.patch.object(update, 'private_remote', return_value='origin')
        self.patch_root.start()
        self.patch_remote.start()
        self.addCleanup(self.patch_root.stop)
        self.addCleanup(self.patch_remote.stop)

    @staticmethod
    def cmd(*args, cwd):
        subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True)

    def advance_remote(self):
        (self.remote / 'project.txt').write_text('updated', encoding='utf-8')
        self.cmd('commit', '-am', 'update', cwd=self.remote)

    def test_ahead_and_uncommitted_changes_are_preserved(self):
        self.assertFalse(update.git_update_status()[-1])
        self.advance_remote()
        self.assertTrue(update.git_update_status()[-1])
        (self.local / 'project.txt').write_text('local changes', encoding='utf-8')
        with mock.patch.object(vcs_disc, 'check', return_value={'ok': True}), self.assertRaises(SystemExit):
            update.apply_git('disc.iso')
        self.assertEqual((self.local / 'project.txt').read_text(encoding='utf-8'), 'local changes')

    def test_fast_forward_and_replace_only_after_builds_succeed(self):
        self.advance_remote()
        build = self.local / 'build'
        staged = build / 'launcher-staged'
        staged.mkdir(parents=True)
        name = 'VCSLauncher.exe' if os.name == 'nt' else 'VCSLauncher'
        (self.local / name).write_bytes(b'old launcher')
        (staged / name).write_bytes(b'new launcher')
        with mock.patch.object(update.tc, 'BUILD', str(build)), \
                mock.patch.object(vcs_disc, 'check', return_value={'ok': True}), \
                mock.patch.object(update.subprocess, 'call', return_value=0), \
                mock.patch.object(update.tc, 'event') as event:
            update.apply_git('disc.iso')
        self.assertEqual((self.local / 'project.txt').read_text(encoding='utf-8'), 'updated')
        self.assertEqual((self.local / name).read_bytes(), b'new launcher')
        self.assertEqual((self.local / (name + '.old')).read_bytes(), b'old launcher')
        self.assertTrue(event.call_args.kwargs['built'])

    def test_rejects_untrusted_remote_url(self):
        self.patch_remote.stop()
        self.cmd('remote', 'set-url', 'origin', 'https://evilgithub.com/ChrisM-Pek/gta-vcs-recomp.git', cwd=self.local)
        with self.assertRaises(SystemExit):
            update.private_remote()
        self.patch_remote.start()

    def test_failed_build_keeps_existing_launcher(self):
        self.advance_remote()
        name = 'VCSLauncher.exe' if os.name == 'nt' else 'VCSLauncher'
        launcher = self.local / name
        launcher.write_bytes(b'old launcher')
        with mock.patch.object(vcs_disc, 'check', return_value={'ok': True}), \
                mock.patch.object(update.subprocess, 'call', return_value=1), self.assertRaises(SystemExit):
            update.apply_git('disc.iso')
        self.assertEqual(launcher.read_bytes(), b'old launcher')


if __name__ == '__main__':
    unittest.main()
