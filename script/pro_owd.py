'''
reference
https://blog.csdn.net/zhang_gang1989/article/details/72884662
https://blog.csdn.net/weixin_38215395/article/details/78679296
'''
#change lineArr[1] the index accoring to the data trace format
import os
def ReadDelayData(fileName):
    sum_owd=0.0;
    sum_lines=0;    
    with open(fileName) as txtData:
        for line in txtData.readlines():
            lineArr = line.strip().split()
            sum_owd+=float(lineArr[2])
            sum_lines+=1;
    return  sum_owd,sum_lines
    
instance=10
flows=3;
fileout="dqc_owd_3.txt"    
name="%s_dqc_%s_owd.txt"
fout=open(fileout,'w')
for case in range(instance):
    total_owd=0.0
    total_lines=0
    average_owd=0.0
    exist=False
    for i in range(flows):
        sum_delay=0.0
        sum_lines=0
        average_owd=0.0
        filename=name%(str(case+1),str(i+1))
        if os.path.exists(filename):
            sum_delay,sum_lines=ReadDelayData(filename)
            total_owd+=sum_delay
            total_lines+=sum_lines
            exist=True
    if exist:
        average_owd=total_owd/total_lines
        fout.write(str(case+1)+"\t")
        fout.write(str(average_owd)+"\n")
    else:
        fout.write(str(case+1)+"\n")
fout.close()        