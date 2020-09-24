// automatically generated by stateify.

package signalfd

import (
	"gvisor.dev/gvisor/pkg/state"
)

func (x *SignalFileDescription) StateTypeName() string {
	return "pkg/sentry/fsimpl/signalfd.SignalFileDescription"
}

func (x *SignalFileDescription) StateFields() []string {
	return []string{
		"vfsfd",
		"FileDescriptionDefaultImpl",
		"DentryMetadataFileDescriptionImpl",
		"NoLockFD",
		"target",
		"mask",
	}
}

func (x *SignalFileDescription) beforeSave() {}

func (x *SignalFileDescription) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.vfsfd)
	m.Save(1, &x.FileDescriptionDefaultImpl)
	m.Save(2, &x.DentryMetadataFileDescriptionImpl)
	m.Save(3, &x.NoLockFD)
	m.Save(4, &x.target)
	m.Save(5, &x.mask)
}

func (x *SignalFileDescription) afterLoad() {}

func (x *SignalFileDescription) StateLoad(m state.Source) {
	m.Load(0, &x.vfsfd)
	m.Load(1, &x.FileDescriptionDefaultImpl)
	m.Load(2, &x.DentryMetadataFileDescriptionImpl)
	m.Load(3, &x.NoLockFD)
	m.Load(4, &x.target)
	m.Load(5, &x.mask)
}

func init() {
	state.Register((*SignalFileDescription)(nil))
}
