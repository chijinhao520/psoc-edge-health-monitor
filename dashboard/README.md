# 健康数据网页

使用 `requirements.txt` 安装依赖后运行 `python server.py`。配置方式见 `.env.example`；不配置AI密钥也可查看数据与导出。

页面可选择设备，查看该设备最近1/6/24小时趋势与有效上传记录占比。图表将无效数据和超过30秒的上传间断显示为空缺；网络读取失败会撤下当前卡片值。切换设备后，晚到的旧数据或AI报告不会覆盖新设备页面。

“导出对照CSV”保留有效标志、服务器接收时间（UTC）及空白参考心率/血氧/备注列。无效值为空，不替换成正常值。同一次设备测量可能多次上传，记录条数与有效占比不等于测量次数或准确率。

相关接口：

- `/api/devices`：设备及最近上传时间。
- `/api/history?device_id=...&hours=24`：单台设备历史；不指定设备时只取最近上传设备。
- `/api/export.csv?device_id=...&hours=24`：同样规则的CSV。

时间范围须大于0且不超过168小时，单次最多10000条，超过时明确要求缩短范围，不静默截断。

部署时同步 `server.py`、`templates/` 和 **`static/`**。Chart.js 4.4.8随项目本地提供，许可证在 `static/vendor/`，无需浏览器访问外网CDN。

离线回归：在仓库根目录执行 `C:/Python314/python.exe -m unittest discover -s dashboard/tests -v`。使用生产路由和内存数据库，17项测试通过，不连接实际设备或外部AI服务。
