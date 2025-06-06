// Copyright 2021 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package runsc

import (
	"os"
	"os/signal"
	"runtime"
	"sync"
	"syscall"

	"github.com/containerd/log"
)

var once sync.Once

func setDebugSigHandler() {
	once.Do(func() {
		dumpCh := make(chan os.Signal, 1)
		signal.Notify(dumpCh, syscall.SIGUSR2)
		go func() {
			buf := make([]byte, 10240)
			for range dumpCh {
				for {
					n := runtime.Stack(buf, true)
					if n >= len(buf) {
						buf = make([]byte, 2*len(buf))
						continue
					}
					log.L.Debugf("User requested stack trace:\n%s", buf[:n])
				}
			}
		}()
		log.L.Debugf("For full process dump run: kill -%d %d", syscall.SIGUSR2, os.Getpid())
	})
}
