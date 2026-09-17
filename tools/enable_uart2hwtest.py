import subprocess

def run(cmd):
    r = subprocess.run(['wsl', '-e', 'bash', '-lc', cmd], capture_output=True, text=True, errors='replace')
    return r.stdout, r.stderr

cfg = '/home/dev/openvela/cmake_out/sf32lb52_devkit_lcd/.config'

# 启用 uart2hwtest
out, err = run(f'cat {cfg}')
with open(r'C:\Users\HUAWEI\Desktop\_cfg', 'w', encoding='utf-8', newline='\n') as f:
    f.write(out)
content = open(r'C:\Users\HUAWEI\Desktop\_cfg', encoding='utf-8').read()
if '# CONFIG_EXAMPLES_UART2HWTEST is not set' in content:
    content = content.replace('# CONFIG_EXAMPLES_UART2HWTEST is not set', 'CONFIG_EXAMPLES_UART2HWTEST=y')
    with open(r'C:\Users\HUAWEI\Desktop\_cfg', 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    run('cp /mnt/c/Users/HUAWEI/Desktop/_cfg ' + cfg)
    print('Enabled UART2HWTEST')
else:
    print('Already enabled or pattern not found')
    for l in content.split('\n'):
        if 'UART2HWTEST' in l:
            print(' ', l)
