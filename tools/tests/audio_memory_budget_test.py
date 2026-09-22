"""Budget guard regressions: catches the linked-BLE failure, not just compile errors."""
import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('budget',Path(__file__).resolve().parents[1]/'check_audio_memory_budget.py')
budget=importlib.util.module_from_spec(spec);spec.loader.exec_module(budget)

def layout(iram=90000,dram=56000):
    return f'app.elf :\nsection size addr\n.iram0.text {iram} {0x40374000}\n.dram0.data {dram} {0x3FC90000}\n.flash.text 3000000 {0x42000000}\n.debug_info 400000 0\n'

class Budget(unittest.TestCase):
    def test_product(self):
        r=budget.inspect(layout(),'app_main T 42000000 20\n')
        self.assertEqual(r['errors'],[]);self.assertEqual(r['internal_static_bytes'],146000)
        self.assertFalse(r['runtime_audio_validated'])
    def test_original_regression(self):
        r=budget.inspect(layout(107351,58076),'app_main T 42000000 20\nnimble_port_init T 42001000 40\n')
        self.assertEqual(len(r['errors']),3)
    def test_alias_padding_is_not_double_counted(self):
        r=budget.inspect(layout()+f".dram0.dummy 90000 {0x3FC88000}\n", "app_main T 42000000")
        self.assertEqual(r["internal_static_bytes"],146000)
        self.assertFalse(r["errors"])
    def test_controller_even_under_size_limit(self):
        self.assertTrue(budget.inspect(layout(),'esp_bt_controller_init T 42001000 40')['errors'])
    def test_flash_growth_is_not_iram(self):
        self.assertFalse(budget.inspect(layout().replace('3000000','8000000'),'app_main T 42000000')['errors'])
    def test_missing_output(self):
        with self.assertRaises(ValueError): budget.inspect('','app_main T 42000000')
        with self.assertRaises(ValueError): budget.inspect(layout(),'')
    def test_wrong_target(self):
        with self.assertRaises(ValueError): budget.inspect('.text 1000 12345','app_main T 3030')
    def test_duplicate_section(self):
        with self.assertRaises(ValueError): budget.inspect(layout()+'.iram0.text 1 1077362688\n','app_main T 42000000')
    def test_hex(self):
        self.assertEqual(budget.sections_from_size('.text 0x10 0x42000000'),[('.text',16,0x42000000)])
    def test_boundary(self):
        self.assertFalse(budget.inspect(layout(95000,56552),'app_main T 42000000')['errors'])
        self.assertTrue(budget.inspect(layout(95000,56553),'app_main T 42000000')['errors'])

if __name__=='__main__': unittest.main()
