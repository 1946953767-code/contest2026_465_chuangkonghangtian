import shutil, subprocess
src = r'C:\Users\HUAWEI\Desktop\ground_station.c'
dst = r'\\wsl.localhost\Ubuntu\home\dev\openvela\apps\examples\uart2hwtest\uart2hwtest.c'
shutil.copy2(src, dst)
print('Copied')
cmd = 'export PATH=/home/dev/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && cd /home/dev/openvela && cmake --build cmake_out/sf32lb52_devkit_lcd 2>&1 | tail -5'
r = subprocess.run(['wsl','-e','bash','-lc',cmd], capture_output=True, text=True, timeout=600, errors='replace')
print(r.stdout[-400:])
