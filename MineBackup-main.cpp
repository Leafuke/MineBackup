#include <iostream> 
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include <thread>
#include <conio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <windows.h> 
using namespace std;
string Gpath,Gpath2[100]; //��Ϸ·�����ָ���Ϸ·�����Ի��������ԣ�
int lang = 0;
string Dialog[5][100] = {
	{"\n���ڽ��������ļ�......\n",
	"���ڳ���Ѱ����֪��Ϸ�浵·��...\n",
	"�ҵ��ٷ�������JAVA�汾�浵���Ƿ����·����(��:1 ��:0)\n",
	"δ����JAVA��浵�������ڣ���֮���ֶ����롣\n",
	"�ҵ��ٷ����������Ұ汾�浵���Ƿ����·����(��:1 ��:0)\n",
	"δ���ֻ��Ұ�浵��\n",
	"�������Ŵ浵�ļ��е��ļ���·�� (дΪһ�У�����ļ���·������$�ָ�): ",
	"������浵���ݴ洢·��:",
	"ʹ�õ������ļ����:0",
	"��ʾ����:1",
	"�浵�ļ���·��:",
	"�浵���ݴ洢·��:",
	"ѹ�����·��:",
	"����ǰ�Ȼ�ԭ:0",
	"�������ö�:0",
	"�ֶ�ѡ��ԭ(Ĭ��ѡ����):0",
	"������ʾ:1",
	"ѹ����ʽ:7z",
	"ѹ���ȼ�:5",
	"�����ı�������(0��ʾ������):0",
	"���ܱ���:0",
	"�����ļ�������ϣ����Թرճ���\n",
	"\n�����´浵:\n\n",
	"�浵����: ",
	"�������ʱ��: ",
	"���Ƿ�ϣ�������д浵���ñ�����(0/1)\n\n",
	"������������Ҫ����Щ�ļ�����һ���������Լ����ı�����\n\n",
	"��ô���Զ��Դ浵�ļ�����Ϊ�����������Ҫ�޸ı��������ڡ����á����ֶ��޸ġ�\n",
	"���������´浵�ı���(������һ������) ",
	"�ļ������Ʋ��ܰ������� \\  /  :  *  ?  \"  <  >  | ������������",
	"\n��⵽�µĴ浵���£�\n",
	"�����Ƿ���������ļ���(0/1)\n",
	"\n���ֶ����£��ڶ�Ӧλ���Ժ�����´浵�ġ���ʵ�����͡�������\n",
	"���ڵ�ʱ���Ѿ�������Ŀ��ʱ��",
	"�����´浵:\n\n",
	"������Ҫ (1)���ݴ浵 (2)�ص� (3)���´浵 (4) �Զ����� ���� (5) ���������ļ� �أ� (�� 1/2/3/4/5)\n",
	"����浵ǰ���������ɱ���:",
	"\n\n�������! ! !\n\n",
	"����浵ǰ���������ɻ�ԭ (ģʽ: ѡȡ���±���) : ",
	"����浵ǰ���������ɻ�ԭ (ģʽ: �ֶ�ѡ��) :",
	"���ݲ����ڣ��޷���ԭ������ܻ�û�н��й�����\n",
	"�����Ǳ��ݴ浵\n\n",
	"���뱸��ǰ���������ɻ�ԭ:",
	"\n\n��ԭ�ɹ�! ! !\n\n",
	"��������Ҫ���ݵĴ浵���:",
	"ÿ�������ӽ��б���: ",
	"�ѽ����Զ�����ģʽ������ʱ����Ϊ����λ: ���ӣ�: ",
	"�밴�����ϵ� 1/2/3/4/5 ��\n\n",
	"\n\n��������� 7-Zip ���и�ѹ�����ݣ�����ļ��������δ��װ 7-Zip �����ȵ����� 7-zip.org ���ػ����� MineBackup �ı�Я�档\n\n",
	"��⵽�ô浵Ϊ��״̬�����ڸô浵�´�����ʱ�ļ��С�\n\n���ڽ��浵�ļ����������ļ����Ƶ�[1��ʱ�ļ���]�У�Ȼ��ʼ����\n�˹������벻Ҫ������\n",
	"\\1��ʱ�ļ���",
	"���ڸ���...",
	"�ļ��������",
	"\n����Ҫ���� (1)һ������ ���� (2)ȫ�Զ�����\n\n",
	"\n���ڴ��������ļ����ļ�����Ϊ",
	"�Ƿ�Ϊ�Զ�����:",
	"��Ҫ���õ������ļ����(���л�ȡ�浵���ƺͱ���):\n",
	"���õ������ļ����:",
	"��Ҫ���ݵڼ����浵:",
	"�������:",
	"����Ҫ (1)��ʱ���� ���� (2)�������\n",
	"������Ҫ��ʲôʱ�䱸��: 1.�������·ݣ�Ȼ��س�(����0��ʾÿ����):",
	"2.���������ڣ�Ȼ��س�(����0��ʾÿ��):",
	"3.������Сʱ��Ȼ��س�(����0��ʾÿСʱ):",
	"4.��������ӣ�Ȼ��س�:",
	"ģʽ:1\n��ʱ��������ʱ�֣�:",
	"������Ҫ������ٷ��ӱ���:",
	"ģʽ:2\n���ʱ��:",
	"\n����\n",
	"�Ƿ��������ģʽ(0/1):",
	"�򿪴浵�ļ���",
	"�򿪱����ļ���",
	"����"
	},
	{"\nCreating configuration file...\n",
	"Trying to find known game save paths...\n",
	"Found official launcher JAVA edition save, should it be added to the path? (Yes:1 No:0)\n",
	"No JAVA edition saves found, if they exist, please enter manually later.\n",
	"Found official launcher Bedrock edition save, should it be added to the path? (Yes:1 No:0)\n",
	"No Bedrock edition saves found.\n",
	"Please enter the folder path for save folders (write in one line, separate multiple folder paths with $): ",
	"Please enter backup storage path:",
	"Configuration file number being used:0",
	"Display language:1",
	"Save folder path:",
	"Backup storage path:",
	"Compression software path:",
	"Restore before backup:0",
	"Toolbox on top:0",
	"Manual selection for restore (default selects latest):0",
	"Process display:1",
	"Compression format:7z",
	"Compression level:5",
	"Number of backups to keep (0 means unlimited):0",
	"Smart backup:0",
	"Configuration file creation complete! You can close the program!\n",
	"\nHere are the saves:\n\n",
	"Save name: ",
	"Last played time: ",
	"Do you want to set aliases for all saves? (0/1)\n\n",
	"Next, you need to give these folders an alias that is easy for you to understand.\n\n",
	"Then the saves will automatically be named after the save folder names, if you need to modify the alias, please manually edit in 'Settings'.\n",
	"Please enter the alias for the following save (can be a description) ",
	"Folder name cannot contain symbols \\ / : * ? \" < > | , please rename",
	"\nNew saves detected as follows:\n",
	"Do you want to update the configuration file? (0/1)\n",
	"\nPlease manually update, add new save's 'real name' and 'alias' after the corresponding position\n",
	"The current time has exceeded the target time",
	"Here are the saves:\n\n",
	"What would you like to do (1)Backup save (2)Restore (3)Update save (4) Automatic backup or (5) Create configuration file? (Press 1/2/3/4/5)\n",
	"Enter the number before the save to complete the backup:",
	"\n\nBackup complete!!!\n\n",
	"Enter the number before the save to restore (Mode: Select latest backup): ",
	"Enter the number before the save to restore (Mode: Manual selection):",
	"Backup does not exist, unable to restore! You may not have performed a backup yet\n",
	"Here are the backed up saves\n\n",
	"Enter the number before the backup to complete the restore:",
	"\n\nRestore successful!!!\n\n",
	"Please enter the number of the save you want to back up:",
	"Backup every few minutes: ",
	"Automatic backup mode activated, backup interval is (unit: minutes): ",
	"Please press the 1/2/3/4/5 key on your keyboard\n\n",
	"\n\nThis program uses 7-Zip for high compression backup, but 7-Zip is not installed on your computer, please download from the official website 7-zip.org or download the portable version of MineBackup.\n\n",
	"Detected that the save is open, a temporary folder has been created under the save.\n\nCopying all files in the save folder to [1 temporary folder], then starting backup\nDo not click randomly during this process\n",
	"\\1 temporary folder",
	"Copying...",
	"File copy completed",
	"\nDo you need to create (1)General configuration or (2)Fully automatic configuration\n\n",
	"\nCreating configuration file, file name is",
	"Is it an automatic configuration:",
	"Configuration file number to be called (from which to get save names and aliases):\n",
	"Called configuration file number:",
	"Which save needs to be backed up:",
	"Backup number:",
	"Do you need (1)Timed backup or (2)Interval backup\n",
	"Enter when you want to back up: 1. Enter the month, then press enter (enter 0 for every month):",
	"2. Enter the day, then press enter (enter 0 for every day):",
	"3. Enter the hour, then press enter (enter 0 for every hour):",
	"4. Enter the minute, then press enter:",
	"Mode:1\nTimed (year/month/day/hour/minute):",
	"Enter how many minutes you want to back up:",
	"Mode:2\nInterval time:",
	"\nError\n",
	"Enable Do Not Disturb mode (0/1):",
	"Open save folder",
	"Open backup folder",
	"Settings"
	}
};
void sprint(string s,int time)//�ӳ���� 
{
	int len=s.size();
	for(int i=0;i<len;i++)
	{
		if(s[i]=='*')
		{
			Sleep(100);
			continue;
		}
		printf("%c",s[i]);
		Sleep(time);
	}
}

inline void neglect(int x)//��ȡ���� x �� 
{
	int num;
	char ch;
	while(num<x)
	{
		ch=getchar();
		if(ch=='\n') ++num;
	}
}

bool isDirectory(const std::string& path)//�ļ����Ƿ���� 
{
#ifdef _WIN32
    DWORD attr = GetFileAttributes(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    DIR* dir = opendir(path.c_str());
    if (dir)
    {
        closedir(dir);
        return true;
    }
    return false;
#endif
}

//�г����ļ��� 
void listSubdirectories(const std::string& folderPath, std::vector<std::string>& subdirectories)
{
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA((folderPath + "\\*").c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)
            {
                subdirectories.push_back(findData.cFileName);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir(folderPath.c_str());
    if (dir)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            {
                subdirectories.push_back(entry->d_name);
            }
        }
        closedir(dir);
    }
#endif
}
//��ȡ�ļ�������޸�ʱ�� 
string getModificationDate(const std::string& filePath)//Folder modification date
{
    string modificationDate;
#ifdef _WIN32
    WIN32_FIND_DATAA fileData;
    HANDLE hFile = FindFirstFileA(filePath.c_str(), &fileData);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        FILETIME ft = fileData.ftLastWriteTime;
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        modificationDate = buffer;

        FindClose(hFile);
    }
#else
    struct stat info;
    if (stat(filePath.c_str(), &info) != 0)
        return modificationDate;

    time_t t = info.st_mtime;
    struct tm* tm = localtime(&t);

    char buffer[256];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm);

    modificationDate = buffer;
#endif

    return modificationDate;
}
string temp[100];
//�г��ļ����ڵ��ļ� 
void ListFiles(const std::string& folderPath) {
    std::string searchPath = folderPath + "\\*.*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
	int i=0;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            	temp[++i] = findData.cFileName;
                cout<< i << ".  " << findData.cFileName << std::endl;
            }
        } while (FindNextFileA(hFind, &findData));

        FindClose(hFind);
    }
}
//Ԥ��ȡ��ֱ��: 
void Qread()
{
	char ch;
	ch=getchar();
	if(ch=='#')
		while(ch!='\n')
			ch=getchar();
	while(ch!=':')
	{
		ch=getchar();
		if(ch=='#') //�����ע���У���ô�������� 
			while(ch!='\n')
				ch=getchar();	
	}
	return ;
}
//��ȡע����ֵ 
string GetRegistryValue(const std::string& keyPath, const std::string& valueName)
{
    HKEY hKey;
    string valueData;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		char buffer[1024];
		DWORD dataSize = sizeof(buffer);
		if (RegGetValueA(hKey, NULL, valueName.c_str(), RRF_RT_ANY, NULL, buffer, &dataSize) == ERROR_SUCCESS) {
			valueData = buffer;
		}
		RegCloseKey(hKey);
	}
	else
	{
		if(access("7z.exe", F_OK) == 0)
		{
			return "7z.exe"; 
		}
		else
		{
			printf("%s",Dialog[lang][48].c_str());
			system("del config.ini");
			system("pause");
			exit(0);
		}
	}
    return valueData;
}
struct names{
	string real,alias;
	int x;
}name[100];
string rname2[30],Bpath,command,yasuo,lv,format;//�浵��ʵ�� �����ļ���·�� cmdָ�� 7-Zip·�� ѹ���ȼ� 
bool prebf,ontop,choice,echos,smart;//�ص�ǰ���� �������ö� �ֶ�ѡ�� ���� ���ܱ��� 
int limitnum;
HWND hwnd;
struct File {
    string name;
    time_t modifiedTime;
};
//�ж��ļ��Ƿ�ռ�� 
bool isFileLocked(const string& filePath)
{
    HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_SHARE_READ, NULL);
    
    if (hFile == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        // ����ļ�������һ�������Զ�ռ��ʽ�򿪣���᷵��ERROR_SHARING_VIOLATION����
        if (error == ERROR_SHARING_VIOLATION)
            return true;
        // �������������Ҫ����ʵ���������
        else
            return false;
    }
    else
    {
        CloseHandle(hFile);
        // �ļ��ɹ����ҹرգ�������ǰû�б�ռ�ã����������˲�䣩
        return false;
    }
}

//�浵�ļ���·��Ԥ���� 
int PreSolve(string s)
{
	int len=s.size(),tt=0;
	for(int i=0;i<len;++i)
	{
		if(s[i]=='$')
		{
			++tt;
			continue;
		}
		Gpath2[tt]+=s[i];
	}
	return tt;
}
//�������������
bool checkupName(string Name)
{
	int len=Name.size();
	for(int i=0;i<len;++i)
		if(Name[i]=='\\' || Name[i]=='/' || Name[i]==':' || Name[i]=='*' || Name[i]=='?' || Name[i]=='"' || Name[i]=='<' || Name[i]=='>' || Name[i]=='|')
			return false;
	return true;
}
//��ⱸ������
void checkup(string folderPath,int limit)
{
	if(limit==0) return ;
	DIR* directory = opendir(folderPath.c_str());
    if (!directory) return ;
    File files;
    struct dirent* entry;
    int checknum=0;
    while ((entry = readdir(directory))) {
    	string fileName = entry->d_name;
		string filePath = folderPath + fileName;
		struct stat fileStat;
		stat(filePath.c_str(), &fileStat);
    	if (S_ISREG(fileStat.st_mode)) {
    		++checknum;//����ǳ����ļ���ͳ���ܱ����� 
    	}
    }
    closedir(directory);
    struct dirent* entry2;
    while (checknum > limit)
    {
    	directory = opendir(folderPath.c_str());//���������ɶ�ȡ�ظ���ֻ��ɾ��һ�Σ����涼�Ҳ��� 
		bool fl=0;
		while ((entry2 = readdir(directory))) {
		    string fileName = entry2->d_name;
		    string filePath = folderPath + fileName;
		    struct stat fileStat;
		    if(!fl) files.modifiedTime=fileStat.st_mtime,fl=1; //����files 
		    if (stat(filePath.c_str(), &fileStat) != -1) {
		    	if (S_ISREG(fileStat.st_mode)) {
			        File file;
			        file.modifiedTime = fileStat.st_mtime;
					if(file.modifiedTime <= files.modifiedTime)
					{
						files.modifiedTime=file.modifiedTime;
						files.name=fileName;
					}
				}
		    }
		}
		string command="del \"" + folderPath + files.name + "\"";
		system(command.c_str());
		--checknum;
		closedir(directory);
	}
	return ;
}
//���ݺ��� 
void Backup(int bf,bool echo)
{
	string folderName = Bpath + "\\" + name[bf].alias; // Set folder name
	// Create a folder using mkdir ()
	mkdir(folderName.c_str());
	
	bool isFileLock = 0;
	if(isFileLocked(name[bf].real+"\\region\\r.0.0.mca"))
	{
		isFileLock = true; 
		printf("%s",Dialog[lang][49].c_str());
		command = "start \"\"  \"" + name[bf].real + "\"";//�����򿪲��ᱨ�� 
		system(command.c_str());
		Sleep(2000);
	    keybd_event(0x11, 0, 0, 0);//Ctrl
		keybd_event(0x41, 0, 0, 0);//A
		keybd_event(0x41, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x43, 0, 0, 0);//C
		keybd_event(0x43, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x11, 0, KEYEVENTF_KEYUP, 0);
		Sleep(500);
		string folderName2 = name[bf].real + Dialog[lang][50];
		mkdir(folderName2.c_str());
		command = "start \"\"  \"" + folderName2 + "\"";
		system(command.c_str());
		Sleep(500);
		keybd_event(0x11, 0, 0, 0);//Ctrl
		keybd_event(0x56, 0, 0, 0);//V
		keybd_event(0x56, 0, KEYEVENTF_KEYUP, 0);//V 
		keybd_event(0x11, 0, KEYEVENTF_KEYUP, 0);//Ctrl
		Sleep(2000);
		keybd_event(0x12, 0, 0, 0);//Alt 
		keybd_event(0x53, 0, 0, 0);//Skip
		keybd_event(0x53, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, KEYEVENTF_KEYUP, 0);
	    // ��ȡ���ƽ��ȴ��ڵľ��
//	    Sleep(10);
	    HWND hForegroundWindow = GetForegroundWindow();
	    cout << Dialog[lang][51] << endl;
	    // ѭ����鴰���Ƿ񻹴���
	    int sumtime=0;
	    while (true) {
	        // �ȴ�һ��ʱ���ټ�飬�����CPUռ��
	        Sleep(2000); // �ȴ�2��
	        sumtime+=2;
	        // ��鴰���Ƿ���Ȼ��Ч ���� ����̫���˸���ûץ������ 
	        if (!IsWindow(hForegroundWindow) || sumtime > 10)
	        {
	        	cout << Dialog[lang][52] << endl;
	        	break; // ���ڹرգ��˳�ѭ��
			}
	    }
		Sleep(1000); 
		keybd_event(0x01, 0, 0, 0);//��� 
		keybd_event(0x01, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, 0, 0);//Alt 
		keybd_event(0x73, 0, 0, 0);//F4
		Sleep(100);
		keybd_event(0x01, 0, 0, 0);//��� 
		keybd_event(0x01, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x73, 0, 0, 0);//F4
		keybd_event(0x73, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, KEYEVENTF_KEYUP, 0);
	}
	else //��¼һ�±���ʱ�����޸�ʱ�䣬�����Ժ����ѹ���Ĺ��� 
	{
		//������QuickBackup�вż�¼ 
	}
	
	time_t now = time(0);
    tm *ltm = localtime(&now);
    string com=asctime(ltm),tmp="";
    
    for(int j=0;j<com.size();++j)
    	if(j>=11 && j<=18)
    		if(j==13 || j==16) tmp+="-";
    		else tmp+=com[j];
    tmp="["+tmp+"]"+name[bf].alias;
    
    string Real = name[bf].real;
    if(isFileLock)
    	Real+=Dialog[lang][50]; 
	if(echo) command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+" \""+Real+"\"\\*";
	else command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+" \""+Real+"\"\\* > nul 2>&1";
//	cout<< endl << command <<endl;//debug 
	system(command.c_str());
	if(echo) command="move "+tmp+".7z "+folderName;
	else command="move "+tmp+".7z "+folderName+" > nul 2>&1";
	system(command.c_str());
	checkup(folderName+"\\",limitnum);
	
	if(isFileLock)
	{
		command = "rmdir /S /Q \"" + Real + "\"";
		system(command.c_str());
	}
//	freopen("CON","w",stdout);
	return ;
}

//���ܱ��ݣ����ٱ��ݣ� 
void QuickBackup(int bf, bool echo)
{
	string folderName = Bpath + "\\" + name[bf].alias; // Set folder name
	mkdir(folderName.c_str());
	
	//��û�м�¼������¼һ�±���ʱ�����޸�ʱ��
	struct stat fileStat;
	string time1 = name[bf].real + "\\region\\r.0.0.mca", addition;
	if(stat(time1.c_str(), &fileStat))//���������mca�ļ�����ô�ǻ��Ұ�浵���������������region�»�����db�� 
	{
		addition = "\\db";
	}
	else
	{
		addition = "\\region";
	}
	string accesss = name[bf].real + "\\Time.txt"; 
	struct dirent* entry;
	time1 = name[bf].real + addition;
	DIR* directory3 = opendir(time1.c_str());
	bool ifbackup = 0;
	if(access(accesss.c_str(), F_OK) != 0) // ���û��Time.txt�����ȼ�¼��Ȼ�󱸷�һ�� 
	{
		ifbackup = true;
		time1 = name[bf].real + "\\Time.txt";
		ofstream newFile(time1);
		time1 = name[bf].real + addition;
	    while ((entry = readdir(directory3))) {
	        string fileName = entry->d_name;
	        string filePath = time1 + "\\" + fileName;
	        if (stat(filePath.c_str(), &fileStat) != -1) {
	            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
					newFile << fileName << " " << fileStat.st_mtime << endl;
	            }
	        }
	    }
	    newFile << "* -1" << endl ;
	    closedir(directory3);	
	}
	
    time_t now = time(0);
	tm *ltm = localtime(&now);
	string com=asctime(ltm),tmp="";
    for(int j=0;j<com.size();++j) //���������ļ��� 
    	if(j>=11 && j<=18)
    		if(j==13 || j==16) tmp+="-";
    		else tmp+=com[j];
    tmp="Q["+tmp+"]"+name[bf].alias; //���ٱ����ļ������Q 
    
    accesss = name[bf].real + "\\Time.txt"; 
//	freopen(accesss.c_str(),"r",stdin);
	ifstream usage;
	usage.open(accesss.c_str());
	string mca[500],number,backlist="";
	long long moditime[500],ii=0;
	while(moditime[ii] != -1) //��Time.txt�ж�ȡʱ�� 
	{
		getline(usage,mca[++ii],' ');
		getline(usage,number,'\n');
		moditime[ii] = stoi(number); //��numberתΪ���� 
	}
		
	
	freopen("CON","r",stdin);
	time1 = name[bf].real + addition;
	directory3 = opendir(time1.c_str()); //����open�������һ��bug 
    while ((entry = readdir(directory3))) {// ���µ�mca�ļ�����
        string fileName = entry->d_name;
        string filePath = time1 + "\\" + fileName;
        if (stat(filePath.c_str(), &fileStat) != -1) {
            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
				bool mcaok = false;
				for(int i = 1 ; i <= ii; ++i)
				{
					if(fileName == mca[i])
					{
						mcaok = true; 
						if(fileStat.st_mtime == moditime[i])
							mcaok = false;
					}
				}
				if(mcaok || ifbackup)
				{
					string Real = name[bf].real + addition +"\\" + fileName;
					backlist = backlist + " \"" + Real + "\"";
				}
            }
        }
    }
    if(backlist.size() <= 5)//û���κθĶ� 
    {
    	puts("No Edition");
    	return ; 
	}
		
    if(echo) command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+backlist;
	else command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+backlist+" > nul 2>&1";
	system(command.c_str());
	if(echo) command="move "+tmp+".7z "+folderName;
	else command="move "+tmp+".7z "+folderName+" > nul 2>&1";
	system(command.c_str());
	checkup(folderName+"\\",limitnum);
	freopen("CON","w",stdout);
	
	
	directory3 = opendir(time1.c_str());
	//�ٴμ�¼����ʱ�� 
	time1 = name[bf].real + "\\Time.txt";
	ofstream newFile(time1);
	time1 = name[bf].real + addition;
    while ((entry = readdir(directory3))) {
        string fileName = entry->d_name;
        string filePath = time1 + "\\" + fileName;
        if (stat(filePath.c_str(), &fileStat) != -1) {
            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
				newFile << fileName << " " << fileStat.st_mtime << endl;
            }
        }
    }
    newFile << "* -1" << endl ;
    closedir(directory3);
	
	return ;
	
}

void Qrestore(int bf,int rs, bool echo)
{
	struct stat fileStat;
	string time1 = name[bf].real + "\\region\\r.0.0.mca", addition;
	if(stat(time1.c_str(), &fileStat))//���������mca�ļ�����ô�ǻ��Ұ�浵���������������region�»�����db�� 
	{
		addition = "\\db";
	}
	else
	{
		addition = "\\region";
	}
	string folderPath = Bpath + "\\" + name[bf].alias + "\\";
	DIR* directory = opendir(folderPath.c_str());
	bool st[100]; //���ڼ�¼�Ƿ��Ѿ���ԭ��
	memset(st,0,sizeof(st)); 
    File files;
    while(files.name.compare(temp[rs]))//Ѱ�ұ�Ŀ��� �� �ı��ݣ�һ������ԭ ֱ��Ŀ�걸�� compare����ֵΪ0��ʾ������� 
    {
    	int tt = 0, ttt;
	    struct dirent* entry;
	    directory = opendir(folderPath.c_str());//�������´� 
	    while ((entry = readdir(directory))) {
	        ++tt;
	        if(st[tt]) continue;	        
			string fileName = entry->d_name;
	        string filePath = folderPath + fileName;
	        struct stat fileStat;
	        if (stat(filePath.c_str(), &fileStat) != -1) {
	            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
					File file;
	                file.name = fileName;
	                file.modifiedTime = fileStat.st_mtime;
					if(file.modifiedTime >= files.modifiedTime)
					{
						files.modifiedTime = file.modifiedTime;
						files.name = file.name;
						ttt = tt;
					}
	            }
	        }
	    }

		command=yasuo+" x "+Bpath+"\\"+name[bf].alias+"\\"+files.name+" -o"+name[bf].real+addition+" -y";
		system(command.c_str());
	    files.modifiedTime = 0;//���ã� 
	    st[ttt] = true;
	    closedir(directory);
	}
    
	sprint(Dialog[lang][43],40);
}

//����ط��ػ������� (�������ã�ֱ��ʹ��getenv)
/*string GetSpPath(string sp)
{
	freopen("temp","w",stdout);
	system(sp.c_str());
	ifstream tmp("temp");
	freopen("CON","w",stdout);
	string ans;
	getline(tmp, ans, '\n');
//    system("del temp"); �����ᵼ��freopen���� 
	return ans;
}*/

//��ʼ����/�������� 
void SetConfig(string filename, bool ifreset, int summ)
{
	//���ڽ����������ļ�����Ϊһ������ SetConfig() 
	freopen("CON","r",stdin);
	cin.clear();
	printf("%s",Dialog[lang][0].c_str()); 
	ofstream newFile(filename);
	if(ifreset)
	{
		printf("%s",Dialog[lang][1].c_str());
		string searchPath = "", searchTemp = "C:\\Users\\" + (string)getenv("USERNAME") + "\\Appdata\\Roaming\\.minecraft\\saves";
		if(isDirectory(searchTemp)) //����%Appdata%�����Ļ�����������Ҫ���⴦�� GetSpPath("echo %Appdata%") ���ڿ���ֱ��getenv 
		{
			printf("%s",Dialog[lang][2].c_str());
			char ifadd = getch();
			if(ifadd == '1')
				searchPath += searchTemp + "$";
		}
		else
			printf("%s",Dialog[lang][3].c_str());//GetSpPath("echo %LOCALAPPDATA%")
		searchTemp = "C:\\Users\\" + (string)getenv("USERNAME") + "\\Appdata\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\games\\com.mojang\\minecraftWorlds";
		if(isDirectory(searchTemp)) 
		{
			printf("%s",Dialog[lang][4].c_str());
			char ifadd = getch();
			if(ifadd == '1')
				searchPath += searchTemp + "$";
		}
		else
			printf("%s",Dialog[lang][5].c_str());
		printf("%s",Dialog[lang][6].c_str());
		getline(cin,Gpath);
		if(Gpath == "") Gpath = searchPath, Gpath.pop_back(); //ɾ������"$" 
		else Gpath = searchPath + Gpath;
		printf("%s",Dialog[lang][7].c_str());
		getline(cin,Bpath);
		summ=PreSolve(Gpath);
//		freopen("CON","w",stdout); 
//		system("del temp"); //һ��ɾ���ͻ������ 
	}
	else
	{
		Gpath="";
		for(int i=0;i<summ;++i)
        	Gpath+=Gpath2[i],Gpath+="$";
        Gpath+=Gpath2[summ];
	}	
	
    if (newFile.is_open()) {
    	newFile << Dialog[lang][8] << endl; //λ��9������������ѡ�� 
    	newFile << Dialog[lang][10] << Gpath << endl;//new
        newFile << Dialog[lang][11] << Bpath << endl;
		string keyPath = "Software\\7-Zip"; 
		string valueName = "Path";
		string softw=GetRegistryValue(keyPath, valueName),softww="";
		for(int i=0;i<softw.size();++i)
			if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
			else softww+=softw[i];
        newFile << Dialog[lang][12] << softww+"7z.exe" << endl;
        newFile << Dialog[lang][13] << endl;
        newFile << Dialog[lang][14] << endl;
        newFile << Dialog[lang][15] << endl;
        newFile << Dialog[lang][16] << endl;
        newFile << Dialog[lang][17] << endl;
        newFile << Dialog[lang][18] << endl;
        newFile << Dialog[lang][19] << endl;
        newFile << Dialog[lang][20] << endl;
	}
	printf("%s",Dialog[lang][22].c_str()); 
	for(int i=0;i<=summ;++i)
	{
		bool ifalias=true; // �Ƿ��ֶ����ñ��� 
		cout << endl; 
		std::vector<std::string> subdirectories;
		listSubdirectories(Gpath2[i], subdirectories);
	    for (const auto& folderName : subdirectories)
	    {
			std::string NGpath = Gpath2[i] + "\\" + folderName;
	        std::string modificationDate = getModificationDate(NGpath);
	        std::cout << Dialog[lang][23] << folderName << endl;
	        std::cout << Dialog[lang][24] << modificationDate << endl;
	        std::cout << "-----------" << endl;
	    }
	    Sleep(1000);
	    sprint(Dialog[lang][25],30);
	    cin>>ifalias; 
	    if(ifalias) sprint(Dialog[lang][26],30);
		else sprint(Dialog[lang][27],30);
		for (const auto& folderName : subdirectories)
	    {
	        string alias;
	        B:
	        if(ifalias)
			{
				cout << Dialog[lang][28] << folderName << endl;
	        	cin >> alias;
			}
			else alias = folderName;
	        if(!checkupName(alias))
			{
				printf("%s",Dialog[lang][29].c_str());
				goto B;
			}
			newFile << folderName << endl << alias << endl;
	    }
	    newFile << "$" << endl;
	}
    newFile << "*" << endl;
    newFile.close();
    printf("%s",Dialog[lang][21].c_str());
    return ;
}

//�����µı����ļ� 
void CreateConfig()
{
	printf("%s",Dialog[lang][53].c_str());
	char ch=getch();
	string folderName,filename = "config1.ini";
	string i="1";
    ifstream file(filename);
    while(true)
    {
    	i[0]+=1;
    	filename="config"+i+".ini";
    	ifstream file(filename);
    	if(!file.is_open()) break;
	}
	if(ch=='1')
	{
		printf("%s %s\n",Dialog[lang][54].c_str(),filename.c_str());
    	ofstream newFile(filename);
    	printf("%s",Dialog[lang][6].c_str());
		getline(cin,Gpath);
		printf("%s",Dialog[lang][7].c_str());
		getline(cin,Bpath);
		for(int i=0;i<=10;++i)
			Gpath2[i]="";
		int summ=PreSolve(Gpath);
        if (newFile.is_open()) {
        	newFile << Dialog[lang][55] << "0" << endl;
        	newFile << Dialog[lang][10] << Gpath2[0];
        	if(summ>1) newFile << '$'; 
        	for(int i=1;i<summ;++i)
        		newFile << Gpath2[i] << '$';
        	if(summ!=0) newFile << Gpath2[summ] << endl;
        	else newFile << endl;
            newFile << Dialog[lang][7] << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
			string softw=GetRegistryValue(keyPath, valueName),softww="";
			for(int i=0;i<softw.size();++i)
				if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
				else softww+=softw[i];
            newFile << Dialog[lang][12] << softww+"7z.exe" << endl;
            newFile << Dialog[lang][13] << endl;
            newFile << Dialog[lang][14] << endl;
            newFile << Dialog[lang][15] << endl;
            newFile << Dialog[lang][16] << endl;
            newFile << Dialog[lang][17] << endl;
	        newFile << Dialog[lang][18] << endl;
	        newFile << Dialog[lang][19] << endl;
	        newFile << Dialog[lang][20] << endl;
    	}
    	printf("%s",Dialog[lang][22].c_str()); 
    	for(int i=0;i<=summ;++i)
    	{
    		cout << endl; 
    		std::vector<std::string> subdirectories;
			listSubdirectories(Gpath2[i], subdirectories);
		    for (const auto& folderName : subdirectories)
		    {
				std::string NGpath=Gpath2[i]+"\\"+folderName;
		        std::string modificationDate = getModificationDate(NGpath);
		        std::cout << Dialog[lang][23] << folderName << endl;
		        std::cout << Dialog[lang][24] << modificationDate << endl;
		        std::cout << "-----------" << endl;
		    }
		    Sleep(2000);
		    sprint(Dialog[lang][26],50);
			for (const auto& folderName : subdirectories)
		    {
		        string alias;
		        cout << Dialog[lang][28] << folderName << endl;
		        cin >> alias;
				newFile << folderName << endl << alias << endl;
		    }
		    newFile << "$" << endl;
		}
	    newFile << "*" << endl;
	    newFile.close();
	    sprint(Dialog[lang][21],10);
        return ;
	}
	else if(ch=='2')
	{
		ofstream newFile(filename);
        newFile << Dialog[lang][55] << "1" << endl;
		int configs;
		printf("%s",Dialog[lang][56].c_str());
		cin>>configs;
		newFile << Dialog[lang][57] << configs << endl;
		printf("%s",Dialog[lang][58].c_str());
		cin>>configs;
		newFile << Dialog[lang][59] << configs << endl;
		printf("%s",Dialog[lang][60].c_str());
		ch=getch();
		if(ch=='1'){
			printf("%s",Dialog[lang][61].c_str());
			int mon,day,hour,min;
			scanf("%d",&mon);
			printf("%s",Dialog[lang][62].c_str());
			scanf("%d",&day);
			printf("%s",Dialog[lang][63].c_str());
			scanf("%d",&hour);
			printf("%s",Dialog[lang][64].c_str());
			scanf("%d",&min);
			newFile << Dialog[lang][65] << mon << " " << day << " " << hour << " " << min << endl;
		} 
		else if(ch=='2'){
			printf("%s",Dialog[lang][66].c_str());
			int detime;
			scanf("%d",&detime);
			newFile << Dialog[lang][67] << detime << endl;
		}
		else{
			printf("%s",Dialog[lang][68].c_str());
			return ;
		}
		printf("%s",Dialog[lang][69].c_str());
		cin>>configs;
		newFile << "Inter:" << configs << "\n*";
		sprint(Dialog[lang][21],10);
		return ;
	}
	else
	{
		printf("%s",Dialog[lang][68].c_str());
		return ;
	}
}
void Main()
{
	LANGID lang_id = GetUserDefaultUILanguage();
	if(lang_id == 2052 || lang_id == 1028) //���ó����� 
		lang = 0;
	else lang = 1; 
	string folderName;
	string filename = "config.ini";
    ifstream file(filename);
    if (!file.is_open()) {
    	//���ڽ����������ļ�����Ϊһ������ SetConfig() 
		SetConfig(filename, true, 0);
        return ;
    }
    
    freopen("config.ini","r",stdin);
    Qread();
    string temps,tempss;
    int configs;
    char configss[100];
	scanf("%d",&configs);
	if(configs!=0)
	{
		temps=to_string(configs); //only c++11
		temps="config"+temps+".ini";
		strcpy(configss,temps.c_str());
		freopen(configss,"r",stdin);
		Qread();
		int auto1;
		cin>>auto1;
		if(auto1)
		{
			int mode,usecf,bfnum,ifinter;
			Qread();
			cin>>usecf;
			Qread();
			cin>>bfnum;
			Qread();
			cin>>mode;
			int bftime,month,day,hour,min;
			if(mode==1)
			{
				Qread(); 
				scanf("%d %d %d %d",&month,&day,&hour,&min);//�������������󣬺������ȷ���� 
				//2023.12.31����������ļ������һ���������� 
			}
			else if(mode==2)
			{
				Qread();
				cin>>bftime;
			}
			Qread();
			cin>>ifinter;
			if(usecf==0) // ʹ��һ�������еĴ浵·�� 
			{
				freopen("config.ini","r",stdin);
//				getline(cin,tmp1);// Problem Here
				neglect(1); 
				Qread();
				char inputs[1000];
				for(int i=0;;++i)
				{
					inputs[i] = getchar();
					if(inputs[i] == '\n'){
						inputs[i] = '\0';break;
					}
				}
				tempss=inputs;
			    int summ=PreSolve(tempss);
			    Qread();
			    memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i] = getchar();
					if(inputs[i] == '\n'){
						inputs[i] = '\0';break;
					}
				}
				Bpath=inputs;
				Qread();
				memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i] = getchar();
					if(inputs[i] == '\n'){
						inputs[i] = '\0';break;
					}
				}
				yasuo = inputs;
				neglect(4);
				Qread();
				memset(inputs,'\0',sizeof(inputs));
				inputs[0] = getchar(),inputs[1] = getchar();
				format = inputs;
				Qread();
				memset(inputs,'\0',sizeof(inputs));
				inputs[0] = getchar();
				lv = inputs;
				Qread();
				cin >> smart;
				Qread();
				cin >> limitnum;
			    int i = 0,ttt = 0;//�浵���� �浵���ڴ浵�ļ������ 
			    inputs[0] = getchar();// addition
			    while(true)
			    {
					memset(inputs,'\0',sizeof(inputs));
					for(int i=0;;++i)
					{
						inputs[i]=getchar();
						if(inputs[i]=='\n'){
							inputs[i]='\0';break;
						}
					}
					name[++i].real=inputs;
			    	if(name[i].real[0]=='*') break;
			    	else if(name[i].real[0]=='$')
			    	{
			    		++ttt,--i;
			    		continue;
					}
			    	name[i].real=Gpath2[ttt]+"\\"+name[i].real;
			    	name[i].x=ttt;
			    	memset(inputs,'\0',sizeof(inputs));
					for(int i=0;;++i)
					{
						inputs[i]=getchar();
						if(inputs[i]=='\n'){
							inputs[i]='\0';break;
						}
					}
					name[i].alias=inputs;
				}
				//���浵�ļ����Ƿ��и��� 
				int ix=0;
				bool ifnew=0; 
				for(int i=0;i<=summ;++i)
				{
					std::vector<std::string> subdirectories;
					listSubdirectories(Gpath2[i], subdirectories);
				    for (const auto& folderName : subdirectories)
				    {
				    	if((Gpath2[i]+"/"+folderName)==name[++ix].real) //��ʵ��һ���򲻸��� ע�⣬����ļ������ȱ�ݵ� 
				    		continue;
						--ix;//��Ϊ���һ�������Աȶ�ʱ-1����δ�ܴ��������� 
				    	//��һ�£����г�
				    	ifnew=true;
						printf("%s",Dialog[lang][30].c_str());
						string NGpath=Gpath2[i]+"/"+folderName;
				        string modificationDate = getModificationDate(NGpath);
				        cout << Dialog[lang][23] << folderName << endl;
				        cout << Dialog[lang][24] << modificationDate << endl;
				        cout << "-----------" << endl;
				    }
				}
				if(ifnew) //����и��£�ѯ���Ƿ���������ļ�
				{
					printf("%s",Dialog[lang][31].c_str());
					char ch;
					ch=getch();
					printf("%s",Dialog[lang][32].c_str());
					if(ch=='1')
						system("start config.ini");
				}
			}
			else
			{
				string temps=to_string(usecf);
			    char configss[10];
				temps="config"+temps+".ini";
				for(int i=0;i<temps.size();++i)
					configss[i] = temps[i];
				freopen(configss,"r",stdin);
				string tmp;
				getline(cin,tmp);
				Qread();
			    getline(cin,temps);
			    int summ=PreSolve(temps);
			    Qread();
			    getline(cin,Bpath);
			    Qread();
			    getline(cin,yasuo);
			    neglect(4);
			    Qread();
				getline(cin,format);
				Qread();
				getline(cin,lv);
				Qread();
				cin>>limitnum;
				Qread();
				cin>>smart;
			    int i=0;
			    int ttt=0;
			    while(true)
			    {
			    	getline(cin,name[++i].real);
			    	if(name[i].real[0]=='*') break;
			    	else if(name[i].real[0]=='$')
			    	{
			    		++ttt,--i;
			    		continue;
					}
			    	name[i].real=Gpath2[ttt]+"/"+name[i].real;
			    	name[i].x=ttt;
			    	getline(cin,name[i].alias);
				}
				//���浵�ļ����Ƿ��и��� 
				int ix=0;
				bool ifnew=0; 
				for(int i=0;i<=summ;++i)
				{
					std::vector<std::string> subdirectories;
					listSubdirectories(Gpath2[i], subdirectories);
				    for (const auto& folderName : subdirectories)
				    {
				    	if((Gpath2[i]+"/"+folderName)==name[++ix].real) //��ʵ��һ���򲻸��� ע�⣬����ļ������ȱ�ݵ� 
				    		continue;
						--ix;//��Ϊ���һ�������Աȶ�ʱ-1����δ�ܴ��������� 
				    	//��һ�£����г�
				    	ifnew=true;
						printf("%s",Dialog[lang][30].c_str());
						string NGpath=Gpath2[i]+"\\"+folderName;
				        string modificationDate = getModificationDate(NGpath);
				        cout << Dialog[lang][23] << folderName << endl;
				        cout << Dialog[lang][24] << modificationDate << endl;
				        cout << "-----------" << endl;
				    }
				}
				if(ifnew) //����и��£�ѯ���Ƿ���������ļ�
				{
					printf("%s",Dialog[lang][31].c_str());
					char ch;
					ch=getch();
					printf("%s",Dialog[lang][32].c_str()); 
					string command="start "+temps;
					if(ch=='1')
						system(command.c_str());
				}
			}
			if(mode==1)
			{
				while(true)
				{
					// ��ȡ��ǰʱ��
				    std::time_t now = std::time(nullptr);
				    std::tm* local_time = std::localtime(&now);
				
				    std::tm target_time = {0};
				    target_time.tm_year = local_time->tm_year; // ��ݴ�1900�꿪ʼ����
				    if(month==0)  target_time.tm_mon = local_time->tm_mon;
				    else target_time.tm_mon = month - 1; // �·ݴ�0��ʼ����
				    if(day==0) target_time.tm_mday = local_time->tm_mday;
				    else target_time.tm_mday = day;
				    if(hour==0) target_time.tm_hour = local_time->tm_hour;
				    else target_time.tm_hour = hour;
				    if(min==0) target_time.tm_min = local_time->tm_min;
				    else target_time.tm_min = min;
				    // �����ǰʱ���Ѿ�������Ŀ��ʱ�䣬��ô�Ͳ���Ҫ�ȴ���
				    A: 
				    if (std::mktime(local_time) > std::mktime(&target_time)) {
				        std::cout << Dialog[lang][33] << std::endl;
						if(min!=0 && hour!=0 && day!=0 && month!=0) return ;
						if(min==0)
						{
							++target_time.tm_min;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_min;
						}
						if(hour==0)
						{
							++target_time.tm_hour;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_hour;
						}
						if(day==0)
						{
							++target_time.tm_mday;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_mday;
						}
						if(month==0)
						{
							++target_time.tm_mon;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_mon;
						}						
				    } else {
				        // ������Ҫ�ȴ���ʱ�䣨��λ���룩
				        std::time_t wait_time = std::difftime(std::mktime(&target_time), std::mktime(local_time));
				        // �ȴ�ָ����ʱ��
				        std::this_thread::sleep_for(std::chrono::seconds(wait_time));
				        if(!smart) Backup(bfnum,false);
				        else QuickBackup(bfnum,false);
				    }
				}
			} 
			else if(mode==2)
			{
				while(true)
				{
				    // ���߳�����ָ����ʱ��
				    std::this_thread::sleep_for(std::chrono::seconds(60*bftime));
				    if(!smart) Backup(bfnum,false);
				    else QuickBackup(bfnum,false);
				}
			}
		}
	}
    Qread();
    getline(cin,temps);
    int summ=PreSolve(temps);
    Qread();
    getline(cin,Bpath);
    Qread();
    getline(cin,yasuo);
    Qread();
    cin>>prebf;
    Qread();
    cin>>ontop;
    Qread();
	cin>>choice;
	Qread();
	cin>>echos;
	Qread();
	getline(cin,format);
	Qread();
	getline(cin,lv);
	Qread();
	cin>>limitnum;
	Qread();
	cin>>smart;
    int i=0,ttt=0;//�浵���� �浵���ڴ浵�ļ������ 
    printf("%s",Dialog[lang][34].c_str());
    char ch=getchar();//DEBUG because getline ...//bug why?now ok?
    while(true)
    {
    	getline(cin,name[++i].real);
    	if(name[i].real[0]=='*') break;
    	else if(name[i].real[0]=='$')
    	{
    		++ttt,--i;
    		continue;
		}
    	name[i].real=Gpath2[ttt]+"\\"+name[i].real;
    	name[i].x=ttt;
    	getline(cin,name[i].alias);
    	printf("%d. ",i);
    	cout<<name[i].alias<<endl;
	}
	freopen("CON","r",stdin);
	//���浵�ļ����Ƿ��и��� 
	int ix=0;
	bool ifnew=0; 
	for(int i=0;i<=summ;++i)
	{
		std::vector<std::string> subdirectories;
		listSubdirectories(Gpath2[i], subdirectories);
	    for (const auto& folderName : subdirectories)
	    {
	    	if((Gpath2[i]+"\\"+folderName)==name[++ix].real) //��ʵ��һ���򲻸��� ע�⣬����ļ������ȱ�ݵ� 
	    		continue;
			--ix;//��Ϊ���һ�������Աȶ�ʱ-1����δ�ܴ��������� 
	    	//��һ�£����г�
	    	ifnew=true;
			printf("%s",Dialog[lang][30].c_str());
			string NGpath=Gpath2[i]+"\\"+folderName;
	        string modificationDate = getModificationDate(NGpath);
	        cout << Dialog[lang][23] << folderName << endl;
	        cout << Dialog[lang][24] << modificationDate << endl;
	        cout << "-----------" << endl;
	    }
	}
	if(ifnew) //����и��£�ѯ���Ƿ���������ļ�
	{
		printf("%s",Dialog[lang][31].c_str());
		char ch;
		ch=getch();
		printf("%s",Dialog[lang][32].c_str());
		if(ch=='1')
			system("start config.ini");
	}
	while(true)
	{
		printf("%s",Dialog[lang][35].c_str());
		char ch;
		ch=getch();
		if(ch=='1')
		{
			printf("%s",Dialog[lang][36].c_str());
			int bf;
			scanf("%d",&bf);
			if(!smart) Backup(bf,echos);
			else QuickBackup(bf,echos);
			sprint(Dialog[lang][37],40);
		}
		else if(ch=='2') //��ԭ�浵 
		{
			int i=0;
		    if(!choice) printf("%s",Dialog[lang][38].c_str());
		    else printf("%s",Dialog[lang][39].c_str());
			int bf;
			scanf("%d",&bf);
			string folderPath = Bpath + "\\" + name[bf].alias + "\\";
			DIR* directory = opendir(folderPath.c_str());
		    if (!directory) {
		        printf("%s",Dialog[lang][40].c_str());
		        return ;
		    }
		    File files;
		    if(!choice)//Ѱ�����±��� 
		    {
			    struct dirent* entry;
			    while ((entry = readdir(directory))) {
			        string fileName = entry->d_name;
			        string filePath = folderPath + fileName;
			        struct stat fileStat;
			        if (stat(filePath.c_str(), &fileStat) != -1) {
			            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
			                File file;
			                file.name = fileName;
			                if(file.name[0] == 'Q') //��������ܱ��ݵĴ浵��������
								continue;
			                file.modifiedTime = fileStat.st_mtime;
							if(file.modifiedTime > files.modifiedTime)
							{
								files.modifiedTime = file.modifiedTime;
								files.name = file.name;
							}
			            }
			        }
			    }
			    closedir(directory);
			}
			else
			{
				string folderName2 = Bpath + "\\" + name[bf].alias;
				printf("%s",Dialog[lang][41].c_str());
				ListFiles(folderName2);
				printf("%s",Dialog[lang][42].c_str());
				int bf2;
				scanf("%d",&bf2);
				files.name = temp[bf2];
				if(files.name[0] == 'Q') //��������ܱ��ݵĴ浵����������Ĵ���
					Qrestore(bf, bf2, false); 
			}
		    if(prebf)
		    	if(!smart) Backup(bf,false);
		    	else QuickBackup(bf,false);
		    if(files.name[0] != 'Q')
		    {
		    	command=yasuo+" x "+Bpath+"\\"+name[bf].alias+"\\"+files.name+" -o"+name[bf].real+" -y";
				system(command.c_str());
				sprint(Dialog[lang][43],30);
			}
		}
		else if(ch=='3')
		{
			SetConfig("config.ini",false,summ); 
		}
		else if(ch=='4')
		{
			printf("%s",Dialog[lang][44].c_str());
			int bf,tim;
			scanf("%d",&bf);
			printf("%s",Dialog[lang][45].c_str());
			scanf("%d",&tim);
			printf("%s%d",Dialog[lang][46].c_str(),tim);
			while(true)
			{
				if(!smart) Backup(bf,false);
				else QuickBackup(bf,false);
				Sleep(tim*60000);
			}
		}
		else if(ch=='5')
		{
			CreateConfig();
		}
		else printf("%s",Dialog[lang][47].c_str());
	}
	std::this_thread::sleep_for(std::chrono::seconds(1));
}

bool isRunning = true;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_COMMAND:
    {
        int buttonId = LOWORD(wParam);

        // Handle button click events
        switch (buttonId)
        {
	        case 1:
        	{
        		int x=0;
	        	while(!Gpath2[x].empty())
	        	{
	        		command="start "+Gpath2[x++];
					system(command.c_str());
				}
	            break;
			}
	        case 2:
	            command="start "+Bpath;
				system(command.c_str());
	            break;
	        case 3:
	            command="start config.ini";
				system(command.c_str());
	            break;
		}
		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	HWND h=GetForegroundWindow();
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "ButtonWindowClass";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,                          
        wc.lpszClassName,          
        "Toolbox",                  
        WS_OVERLAPPEDWINDOW,          
        CW_USEDEFAULT, CW_USEDEFAULT, 
        480, 85,              
        NULL,                     
        NULL,                     
        hInstance,
        NULL
    );
    CreateWindow("button", Dialog[lang][70].c_str(),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, 10, 120, 35,
        hwnd, (HMENU)1, hInstance, NULL);

    CreateWindow("button", Dialog[lang][71].c_str(),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        150, 10, 120, 35,
        hwnd, (HMENU)2, hInstance, NULL);

    CreateWindow("button", Dialog[lang][72].c_str(),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        280, 10, 80, 35,
        hwnd, (HMENU)3, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);    
    std::thread MainThread(Main);
    MSG msg = {};
    
    //�߳����ߣ�Ϊ�˵ȴ�ontop��ȡ��� 
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(ontop)
		SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE);//Top the window
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    isRunning = false;
    MainThread.join();
    return 0;
}
