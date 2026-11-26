import unittest
import numpy as np
import warp as wp
from artistic_warp_v31d1112 import StrokeSession

class TestWarp(unittest.TestCase):
    def run_device(self, device):
        s = StrokeSession([0,2,4,6],[1,2,0,2,0,1],device)
        p = np.array([[0,0,0],[1,0,0],[0,1,0]],np.float32)
        n = np.array([[0,0,1]]*3,np.float32)
        w = [1,1,0]
        for dose in [.01,.1,1.0,0.0]:
            result = s.evaluate(p,n,w,[dose,dose,dose],1.,.5)
            d = min(-np.expm1(-dose*.5),.1)
            np.testing.assert_allclose(result,[[d,0,0],[-d,0,0],[0,0,0]],atol=2e-7)
        with self.assertRaises(ValueError):
            s.evaluate(p,n,w,[-1,0,0],1.,.5)
    def test_cpu(self):
        self.run_device("cpu")
    @unittest.skipUnless(wp.is_cuda_available(),"CUDA unavailable")
    def test_cuda(self):
        self.run_device("cuda:0")

if __name__ == "__main__":
    unittest.main()
