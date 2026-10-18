"""One physical Bluetooth owner across all copies of the emulator."""
import msvcrt
from pathlib import Path
import tempfile


class RadioBusy(RuntimeError):
    pass


class RadioGuard:
    def __enter__(self):
        folder = Path(tempfile.gettempdir()) / 'xiaomi-q66-radio'
        folder.mkdir(exist_ok=True)
        self.stream = (folder / 'radio.lock').open('a+b')
        self.stream.seek(0)
        try:
            msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as exc:
            self.stream.close()
            raise RadioBusy('已有模拟器占用电脑蓝牙，请先关闭原工程或其他文件夹中的模拟器。') from exc
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.stream.seek(0)
        msvcrt.locking(self.stream.fileno(), msvcrt.LK_UNLCK, 1)
        self.stream.close()
