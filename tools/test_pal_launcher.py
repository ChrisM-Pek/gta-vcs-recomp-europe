#!/usr/bin/env python3
"""Checks region selection and the PAL generation stamp without a game disc."""
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import build_pal
import doctor
import vcs_disc


class PalLauncherTests(unittest.TestCase):
    def test_rejects_wrong_region_executable_hash(self):
        image = mock.Mock()
        image.find.side_effect = lambda name: {'dir': False, 'name': name}
        image.read_file.side_effect = lambda rec: (b'BOOT2 = cdrom0:\\SLES_546.22;1' if rec['name'] == 'SYSTEM.CNF'
                                                   else b'not the expected executable')
        with mock.patch('os.path.isfile', return_value=True), mock.patch.object(vcs_disc, 'IsoImage', return_value=image):
            result = vcs_disc.check('game.iso')
        self.assertFalse(result['ok'])
        self.assertEqual(result['code'], 'unsupported_build')

    def test_pal_stamp_requires_generated_files_and_matching_inputs(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(build_pal.tc, 'BUILD', tmp), \
                mock.patch.object(build_pal, 'STAMP', str(Path(tmp) / 'stamp.json')), \
                mock.patch.object(build_pal, 'inputs_hash', return_value='expected'):
            self.assertIsNotNone(build_pal.why_recompile())
            stage = Path(tmp) / 'analysis' / 'pal_stage'
            stage.mkdir(parents=True)
            (stage / 'register_functions.cpp').touch()
            (stage.parent / 'pal_vu1.cpp').touch()
            Path(build_pal.STAMP).write_text('{"inputs":"expected"}', encoding='utf-8')
            self.assertIsNone(build_pal.why_recompile())
            Path(build_pal.STAMP).write_text('{"inputs":"outdated"}', encoding='utf-8')
            self.assertIsNotNone(build_pal.why_recompile())

    def test_doctor_rejects_an_executable_from_another_region(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(doctor.tc, 'BUILD', tmp), \
                mock.patch.object(doctor.tc, 'VCS_EXE', str(Path(tmp) / 'vcs.exe')), \
                mock.patch.object(doctor.tc, 'source_version', return_value='source'), \
                mock.patch.object(doctor, 'newest_input', return_value=(0, '')), \
                mock.patch.object(build_pal, 'why_recompile', return_value=None):
            Path(doctor.tc.VCS_EXE).touch()
            (Path(tmp) / 'build_info.json').write_text('{"region":"usa","source":"source"}', encoding='utf-8')
            result = doctor.check_build(disc={'ok': True, 'boot': 'SLES_546.22'})
            self.assertFalse(result['current'])
            self.assertIn('the game was built for another disc region', result['reasons'])


if __name__ == '__main__':
    unittest.main()
